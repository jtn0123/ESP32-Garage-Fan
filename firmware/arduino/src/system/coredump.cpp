// See coredump.h.
#include "system/coredump.h"

#include <Arduino.h>

#include <cstdio>
#include <cstring>

#include "esp_core_dump.h"
#include "esp_partition.h"
#include "system/eventlog.h"

namespace coredump {
namespace {

/**
 * Xtensa EXCCAUSE, named.
 *
 * The number alone is not actionable; the name usually is. LoadProhibited and
 * StoreProhibited mean a bad pointer was read or written (a null or a freed
 * object), IllegalInstruction usually means a corrupted return address --
 * which is what a smashed stack looks like from here, and is exactly the class
 * of fault this device has already hit once.
 */
const char* exc_cause_name(uint32_t cause) {
  switch (cause) {
    case 0:
      return "IllegalInstruction";
    case 1:
      return "Syscall";
    case 2:
      return "InstructionFetchError";
    case 3:
      return "LoadStoreError";
    case 4:
      return "Level1Interrupt";
    case 5:
      return "Alloca";
    case 6:
      return "IntegerDivideByZero";
    case 8:
      return "Privileged";
    case 9:
      return "LoadStoreAlignment";
    case 12:
      return "InstrPIFDataError";
    case 13:
      return "LoadStorePIFDataError";
    case 14:
      return "InstrPIFAddrError";
    case 15:
      return "LoadStorePIFAddrError";
    case 16:
      return "InstTLBMiss";
    case 17:
      return "InstTLBMultiHit";
    case 18:
      return "InstFetchPrivilege";
    case 20:
      return "InstFetchProhibited";
    case 24:
      return "LoadStoreTLBMiss";
    case 25:
      return "LoadStoreTLBMultiHit";
    case 26:
      return "LoadStorePrivilege";
    case 28:
      return "LoadProhibited";
    case 29:
      return "StoreProhibited";
    default:
      return "unknown";
  }
}

/**
 * Is there a coredump partition at all?
 *
 * THIS BOARD HAS NONE. It is flashed with tinyuf2-partitions-4MB.csv
 * (nvs / otadata / ota_0 / ota_1 / uf2 / ffat), not the IDF default that
 * carries a 64 KB `coredump` entry -- which is why /api/state reports its slot
 * as "ota_0" rather than "app0".
 *
 * Calling the esp_core_dump API without that partition is what took the board
 * off the network on 1.14.31: the check ran in setup() before WiFi, so a fault
 * there cost the radio and with it every means of recovery except the
 * boot-health rollback. Everything in this file now refuses to touch the API
 * unless the partition is really there, and the caller runs it only after the
 * network is up.
 *
 * Adding the partition means rewriting the partition table, which OTA cannot
 * do -- it needs a USB flash. Until then this module reports honestly that no
 * dump can exist rather than pretending to look for one.
 */
bool partition_exists() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
                                  NULL) != nullptr;
}

bool get(esp_core_dump_summary_t* s) { return present() && esp_core_dump_get_summary(s) == ESP_OK; }

}  // namespace

bool supported() { return partition_exists(); }

bool present() { return partition_exists() && esp_core_dump_image_check() == ESP_OK; }

void one_line(char* out, size_t cap) {
  if (!out || cap == 0)
    return;
  out[0] = '\0';
  esp_core_dump_summary_t s;
  if (!get(&s))
    return;
  snprintf(out, cap, "panic in %s: %s at 0x%08lx (addr 0x%08lx, depth %lu%s)", s.exc_task,
           exc_cause_name(s.ex_info.exc_cause), (unsigned long)s.exc_pc,
           (unsigned long)s.ex_info.exc_vaddr, (unsigned long)s.exc_bt_info.depth,
           s.exc_bt_info.corrupted ? ", CORRUPTED" : "");
}

size_t to_json(char* out, size_t cap) {
  if (!out || cap == 0)
    return 0;
  esp_core_dump_summary_t s;
  if (!get(&s)) {
    // Distinguish "this board cannot store dumps" from "nothing has crashed".
    // Reporting both as present:false sent the reader looking for a crash that
    // could never have been recorded.
    const int n = snprintf(out, cap, "{\"present\":false,\"supported\":%s,\"note\":\"%s\"}",
                           supported() ? "true" : "false",
                           supported() ? "no core dump stored"
                                       : "this board's partition table has no coredump partition; "
                                         "adding one needs a USB flash, not OTA");
    return n > 0 ? static_cast<size_t>(n) : 0;
  }
  int n = snprintf(out, cap,
                   "{\"present\":true,\"task\":\"%s\",\"cause\":%lu,\"cause_name\":\"%s\","
                   "\"pc\":%lu,\"vaddr\":%lu,\"corrupted\":%s,\"elf_sha256\":\"%s\","
                   "\"backtrace\":[",
                   s.exc_task, (unsigned long)s.ex_info.exc_cause,
                   exc_cause_name(s.ex_info.exc_cause), (unsigned long)s.exc_pc,
                   (unsigned long)s.ex_info.exc_vaddr, s.exc_bt_info.corrupted ? "true" : "false",
                   reinterpret_cast<const char*>(s.app_elf_sha256));
  if (n < 0 || static_cast<size_t>(n) >= cap)
    return 0;
  // Decimal, not hex: the console renders these and JSON has no hex literal.
  // scripts/decode_crash.py turns them back into file:line.
  const uint32_t depth = s.exc_bt_info.depth > 16 ? 16 : s.exc_bt_info.depth;
  for (uint32_t i = 0; i < depth && static_cast<size_t>(n) < cap; i++) {
    n += snprintf(out + n, cap - n, i ? ",%lu" : "%lu", (unsigned long)s.exc_bt_info.bt[i]);
  }
  if (n < 0 || static_cast<size_t>(n) >= cap)
    return 0;
  n += snprintf(out + n, cap - n, "]}");
  return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

bool image_range(size_t* addr, size_t* size) {
  if (!addr || !size)
    return false;
  return esp_core_dump_image_get(addr, size) == ESP_OK;
}

bool read_image(size_t off, uint8_t* buf, size_t len) {
  const esp_partition_t* p =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
  if (!p || !buf)
    return false;
  return esp_partition_read(p, off, buf, len) == ESP_OK;
}

bool erase() { return esp_core_dump_image_erase() == ESP_OK; }

void log_at_boot() {
  static bool done = false;
  if (done)
    return;
  done = true;
  if (!supported()) {
    eventlog::log("crash", "no coredump partition on this board; panics keep only the breadcrumb");
    return;
  }
  char line[160];
  one_line(line, sizeof(line));
  if (line[0] == '\0')
    return;
  // On the tape, right after the boot line, so "cause=panic" and the reason
  // for it sit together in /api/events.
  eventlog::log("crash", "%s", line);
  size_t addr = 0, size = 0;
  if (image_range(&addr, &size))
    eventlog::log("crash", "dump %u bytes at 0x%06x; GET /api/crash", (unsigned)size,
                  (unsigned)addr);
}

}  // namespace coredump
