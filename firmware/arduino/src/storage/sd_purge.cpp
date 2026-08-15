// The purge machinery: the bounded iterative walk that deletes the card's
// contents, with its PSRAM scratch. Split from sdcard.cpp when it passed
// 700 lines; the walk's history (a recursive version that smashed the stack,
// a DRAM batch that starved the mount guard) is retold inline below because
// each detail is a rule the next editor must not un-learn.
#include <Arduino.h>
#include <SD.h>

#include <cstdio>
#include <cstring>

#include "esp_task_wdt.h"
#include "storage/purge_logic.h"
#include "storage/sdcard.h"
#include "system/crashlog.h"
#include "system/eventlog.h"

namespace sdcard {

namespace {
// Bound on entries touched in one purge. A card with a pathological directory
// count must not hold the loop indefinitely; the caller is told it stopped
// early and can run it again.
constexpr uint32_t kPurgeMaxEntries = 20000;

// Iterative directory walk. NOT recursive, and the buffers are static.
//
// The first version recursed with a 24 x 128-byte batch buffer as a LOCAL --
// ~3 KB of stack per level against an 8 KB loop-task stack, so a few levels
// deep it smashed the stack and panicked the board. It did exactly that on the
// deployed unit (boot 42, cause=panic, 2026-08-11) and reclaimed nothing.
//
// So: one shared batch buffer, one bounded queue of directories still to
// visit, no recursion, and therefore a stack cost that does not depend on how
// deep the card's tree happens to be.
//
// Deleting is still collect-then-delete against a CLOSED handle: FATFS does
// not promise consistent iteration while entries are being removed from the
// directory being iterated.
// A far larger batch than the walk started with, and it lives in PSRAM only
// while a purge is running.
//
// Two problems with the first version, both real:
//
//  * 7.7 KB of PERMANENT .bss on a board that has already fought heap
//    exhaustion, and whose mount guard refuses to keep the card mounted unless
//    10 KB stays contiguous afterwards. The graph scratch was moved to PSRAM
//    for exactly this reason; putting the purge's buffers in DRAM undid that
//    lesson in the same file.
//
//  * Rescanning was quadratic. Each pass reopened the directory, enumerated
//    from entry zero and removed at most kBatch files, so one directory of N
//    entries cost about N^2/(2*kBatch) openNextFile calls. The measured card
//    held 208 entries so it never showed; at the 20000-entry budget it is on
//    the order of ten million directory reads, with the task watchdog deleted
//    for the duration -- the board would sit unresponsive rather than reset.
//    A batch this size collapses a realistic directory into a handful of
//    passes.
// The batch is sized at RUNTIME, because the two memory situations differ by
// three orders of magnitude.
//
// kBatchPsram entries is ~200 KB, which PSRAM serves without noticing. Asking
// the INTERNAL heap for that is hopeless -- this board's largest contiguous
// block measures ~17 KB once WiFi is up, and the mount guard un-mounts the
// card below 10 KB -- so a fixed-size struct with an internal-heap fallback
// was a fallback that could never fire: on a board without PSRAM the
// allocation simply failed and purge() deleted nothing while reporting
// complete=false. A small batch just costs more passes, and scan_dir already
// handles a partial one.
constexpr size_t kBatchPsram = 2048;
constexpr size_t kBatchDram = 48;  // ~4.6 KB, leaves the mount guard its room
constexpr size_t kPathMax = purge_logic::kMaxPathLen;

// Hard ceiling on entries ENUMERATED by one scan_dir call, accepted or not.
// The batch cap alone does not bound the loop, because a rejected path does not
// fill a slot. This guarantees the call returns; anything past it is left for
// the next pass, and too_long already records that the walk was not exhaustive.
constexpr uint32_t kMaxScanPerCall = 20000;

// One fixed-width path slot. Named rather than spelled inline: the cast
// `static_cast<char (*)[kPathMax]>` is formatted differently by clang-format 18
// (CI) and 22 (dev machines), so the build failed on whitespace nobody wrote.
using PathSlot = char[kPathMax];

struct PurgeScratch {
  size_t cap = 0;
  uint32_t too_long = 0;  // entries whose path did not fit kPathMax
  PathSlot* batch = nullptr;
  bool* batch_dir = nullptr;
  purge_logic::Queue dirq;
};

PurgeScratch g_scratch;

// One block for both arrays: fewer allocations to fail, and nothing to leak
// halfway through.
bool scratch_alloc() {
  if (g_scratch.batch)
    return true;
  struct Attempt {
    size_t cap;
    uint32_t caps;
  };
  static const Attempt kTries[] = {{kBatchPsram, MALLOC_CAP_SPIRAM}, {kBatchDram, MALLOC_CAP_8BIT}};
  for (const Attempt& a : kTries) {
    const size_t cap = a.cap;
    const size_t bytes = cap * kPathMax + cap;
    void* m = heap_caps_calloc(1, bytes, a.caps);
    if (!m)
      continue;
    g_scratch.cap = cap;
    g_scratch.batch = static_cast<PathSlot*>(m);
    g_scratch.batch_dir = reinterpret_cast<bool*>(static_cast<char*>(m) + cap * kPathMax);
    g_scratch.dirq.reset();
    g_scratch.too_long = 0;
    return true;
  }
  eventlog::log("sd", "purge: no memory for a scratch batch (largest=%lu)",
                (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  return false;
}

void scratch_free() {
  if (!g_scratch.batch)
    return;
  heap_caps_free(g_scratch.batch);
  g_scratch.batch = nullptr;
  g_scratch.batch_dir = nullptr;
  g_scratch.cap = 0;
}

// Fill the batch from `path`. Returns entries read; 0 means the directory is
// empty (or gone). Deleting still happens against a CLOSED handle: FATFS does
// not promise consistent iteration while entries are being removed from the
// directory being walked.
size_t scan_dir(const char* path) {
  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) {
    if (dir)
      dir.close();
    return 0;
  }
  size_t n = 0;
  // Every ENUMERATED entry counts against the walk, not just the accepted ones.
  // A rejected path used to `continue` without touching n, so a directory full
  // of over-long names scanned without any bound and never reached the watchdog
  // reset below -- and purge() removes this task from the watchdog for its
  // duration, so nothing else would have caught it either. That is the same
  // shape as the hang that bricked the board: an unbounded loop with no timer
  // behind it.
  uint32_t seen = 0;
  while (n < g_scratch.cap && seen < kMaxScanPerCall) {
    File e = dir.openNextFile();
    if (!e)
      break;
    seen++;
    if ((seen & 0x3F) == 0)
      esp_task_wdt_reset();
    // A path that does not FIT must not be acted on. snprintf truncates
    // silently, and a truncated path is a different, usually shorter, path --
    // so the delete below would remove the wrong entry, or the traversal would
    // descend somewhere else entirely. Skip it and count it; purge() then
    // reports incomplete rather than claiming a card it never fully walked.
    const int w = snprintf(g_scratch.batch[n], kPathMax, "%s", e.path());
    const bool fits = w > 0 && static_cast<size_t>(w) < kPathMax;
    g_scratch.batch_dir[n] = e.isDirectory();
    e.close();
    if (!fits) {
      g_scratch.too_long++;
      continue;  // n is not advanced: the slot is reused by the next entry
    }
    // Queue subdirectories for their own pass rather than descending here.
    // Queue::push owns the dedupe and the bound -- see purge_logic.h for why
    // both matter (a parent rescanned for more files sees its children every
    // time).
    if (g_scratch.batch_dir[n])
      g_scratch.dirq.push(g_scratch.batch[n]);
    n++;
  }
  dir.close();
  return n;
}

}  // namespace

PurgeResult purge() {
  PurgeResult r;
  if (!ok())
    return r;
  Serial.println("[SD] purge requested");
  esp_task_wdt_delete(NULL);  // deleting tens of thousands of entries takes time
  crashlog::rtc_sd_sentinel = crashlog::kSdSentinelMagic;
  CRUMB("sd_purge");
  const uint32_t before = free_mb();
  uint32_t budget = kPurgeMaxEntries;
  if (!scratch_alloc()) {
    crashlog::rtc_sd_sentinel = 0;
    CRUMB_CLEAR();
    esp_task_wdt_add(NULL);
    return r;  // complete stays false; nothing was touched
  }

  // Breadth-first over a bounded queue. Files go immediately; directories are
  // queued and emptied in their own pass, then removed deepest-first at the
  // end (a later queue entry is always at least as deep as an earlier one, so
  // reverse order suffices).
  purge_logic::Queue& dirq = g_scratch.dirq;
  dirq.reset();
  dirq.push("/");
  for (size_t qi = 0; qi < dirq.size() && budget > 0; qi++) {
    for (;;) {
      const size_t n = scan_dir(dirq.at(qi));
      size_t removed = 0;
      for (size_t i = 0; i < n && budget > 0; i++) {
        budget--;
        esp_task_wdt_reset();
        if (g_scratch.batch_dir[i])
          continue;  // emptied later from the queue
        if (SD.remove(g_scratch.batch[i])) {
          r.files++;
          removed++;
        }
      }
      // Nothing removed means every remaining entry is a queued directory (or
      // the card refused): rescanning returns the same names forever.
      if (purge_logic::scan_exhausted(n, removed))
        break;
    }
  }
  // Deepest-first so a parent is only removed once its children are gone.
  for (size_t i = dirq.size(); i-- > 1;) {
    esp_task_wdt_reset();
    if (SD.rmdir(dirq.at(i)))
      r.dirs++;
  }
  r.skipped_dirs = dirq.skipped();
  r.too_long = g_scratch.too_long;
  if (r.too_long)
    eventlog::log("sd", "purge: %lu entries have paths longer than %u; not touched",
                  (unsigned long)r.too_long, (unsigned)kPathMax);
  if (r.skipped_dirs)
    eventlog::log("sd", "purge: %u dirs beyond the queue bound; run again",
                  (unsigned)r.skipped_dirs);
  // Verify by looking, not by inferring from the budget. "We did not run out
  // of allowance" is not the same claim as "the card is empty" -- a directory
  // that refused every delete also leaves budget to spare, and the endpoint
  // would have told the operator their card was cleared.
  //
  // But the firmware writes to its OWN log while purging, so /events.log is
  // back before this check runs. Counting that as "not finished" is how the
  // first real purge reported (partial) and told the operator to run it again
  // after it had already reclaimed all 28 GB. Ours do not count against it.
  r.remaining = 0;
  bool verified = false;
  File root = SD.open("/");
  if (root && root.isDirectory()) {
    verified = true;
    for (;;) {
      File e = root.openNextFile();
      if (!e)
        break;
      // Match on the BASENAME, case-insensitively. FAT is case-insensitive and
      // the driver can hand back 8.3 short names in upper case, so comparing
      // full paths with strcmp missed our own files and reported them as
      // foreign leftovers.
      const bool ours = purge_logic::is_our_file(e.path());
      if (!ours) {
        if (r.remaining == 0)
          snprintf(r.leftover, sizeof(r.leftover), "%s", e.path());
        r.remaining++;
      }
      e.close();
    }
  }
  if (root)
    root.close();
  // `verified` matters: if the root would not open, the loop never ran and
  // r.remaining stayed 0, which would have made this true and told the operator
  // "card contents deleted" on the strength of a check that did not happen --
  // the exact inference the comment above rejects.
  r.complete =
      verified && r.too_long == 0 && purge_logic::complete(budget, r.skipped_dirs, r.remaining);
  // Name the first survivor. "Some entries could not be deleted" with no
  // indication of WHICH is the kind of report that cannot be acted on.
  if (r.remaining)
    eventlog::log("sd", "purge left %lu entr%s, first: %s", (unsigned long)r.remaining,
                  r.remaining == 1 ? "y" : "ies", r.leftover);
  const uint32_t after = free_mb();
  r.freed_mb = after > before ? after - before : 0;
  scratch_free();
  crashlog::rtc_sd_sentinel = 0;
  CRUMB_CLEAR();
  esp_task_wdt_add(NULL);
  eventlog::log("sd", "purge: %lu files %lu dirs, +%lu MB free%s", (unsigned long)r.files,
                (unsigned long)r.dirs, (unsigned long)r.freed_mb, r.complete ? "" : " (partial)");
  return r;
}

}  // namespace sdcard
