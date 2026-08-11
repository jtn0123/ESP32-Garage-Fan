#pragma once
// The bookkeeping behind sdcard::purge(), separated so the host can test it.
//
// Two bugs lived here, both of which reached the deployed board on
// 2026-08-11:
//
//  1. The walk was recursive and held its batch buffer as a LOCAL -- about
//     3 KB of stack per level against an 8 KB loop-task stack. It overflowed
//     and panicked the board, reclaiming nothing. Hence Queue: the traversal
//     is now breadth-first over this fixed structure, so stack cost does not
//     depend on how deep the card's tree happens to be. kMaxPathLen and the
//     sizeof assertion in the tests exist to keep it that way.
//
//  2. Directories are only unlinked in the final rmdir sweep, so every rescan
//     of a parent saw its children again and re-queued them. The queue filled
//     with duplicates of the same few paths, hit its bound, and purge()
//     reported work it had not done. Hence push()'s dedupe.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace purge_logic {

constexpr size_t kMaxPathLen = 96;
constexpr size_t kMaxDirs = 64;

/**
 * Bounded, de-duplicating FIFO of directories still to visit.
 *
 * Deliberately a plain fixed array: it lives in BSS, never on the stack, and
 * has no allocation to fail partway through a delete sweep.
 */
class Queue {
 public:
  /**
   * Add `path` unless it is already queued or the bound is reached.
   *
   * Returns true when it was added. A rejected push because the queue is full
   * increments skipped(), which purge() reports rather than silently claiming
   * a complete sweep -- a duplicate is not skipped work and does not count.
   */
  bool push(const char* path) {
    if (!path || !*path)
      return false;
    for (size_t i = 0; i < n_; i++) {
      if (strcmp(paths_[i], path) == 0)
        return false;  // already queued; not an overflow
    }
    if (n_ >= kMaxDirs) {
      skipped_++;
      return false;
    }
    snprintf(paths_[n_], kMaxPathLen, "%s", path);
    n_++;
    return true;
  }

  const char* at(size_t i) const { return i < n_ ? paths_[i] : ""; }
  size_t size() const { return n_; }
  size_t skipped() const { return skipped_; }

  void reset() {
    n_ = 0;
    skipped_ = 0;
  }

 private:
  char paths_[kMaxDirs][kMaxPathLen];
  size_t n_ = 0;
  size_t skipped_ = 0;
};

/**
 * Should the rescan loop for one directory stop?
 *
 * A pass that removed nothing means every remaining entry is a queued
 * directory (or the card refused them), so rescanning returns the same names
 * forever. Without this the walk spins on a directory it cannot empty.
 */
inline bool scan_exhausted(size_t entries_seen, size_t entries_removed) {
  return entries_seen == 0 || entries_removed == 0;
}

/**
 * Is a root entry one of ours, and therefore not evidence of an incomplete
 * purge?
 *
 * Matched on the BASENAME and case-insensitively: FAT is case-insensitive and
 * the driver hands back 8.3 short names in upper case, so comparing full paths
 * with strcmp reported the fan's own freshly-recreated log as a foreign
 * leftover -- which is how the first real purge reclaimed 28 GB and then told
 * the operator to run it again.
 */
inline bool is_our_file(const char* path) {
  if (!path)
    return false;
  const char* slash = strrchr(path, '/');
  const char* base = slash ? slash + 1 : path;
  return strcasecmp(base, "events.log") == 0 || strcasecmp(base, "events.old") == 0 ||
         strncasecmp(base, "climate-", 8) == 0;
}

/** Did the sweep finish: nothing foreign left, no bound hit, budget to spare. */
inline bool complete(uint32_t budget_left, size_t skipped_dirs, uint32_t remaining) {
  return budget_left > 0 && skipped_dirs == 0 && remaining == 0;
}

}  // namespace purge_logic
