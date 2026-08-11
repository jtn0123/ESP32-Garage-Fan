#pragma once
// Reading the core dump the panic handler already wrote.
//
// This exists because of a specific blind spot. When this board panics, ESP-IDF
// prints the register set and backtrace over UART -- and the ESP32-S2's USB CDC
// drops serial output unless a DTR-asserting terminal is attached, which in a
// garage it never is. So every panic reached us as the single word "panic" in
// crashlog, with no PC, no task, and no cause. Two purge panics were diagnosed
// by reading source and guessing; one of them is still unexplained.
//
// The prebuilt esp32s2 libs ship with CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
// and ELF format, and the IDF DEFAULT partition table carries a 64 KB
// `coredump` entry -- which is what made this look like free evidence lying
// unread in flash. It is not: see the caveat below. This module is written so
// it becomes useful the moment a board does have the partition, and reports
// the truth on one that does not.
//
// esp_core_dump_get_summary() decodes the important part on-device: the task
// that died, the exception cause and faulting address, the PC, and a
// backtrace. That is small enough to put on the flight recorder and serve as
// JSON, so a panic becomes diagnosable over the network instead of requiring
// a laptop, a cable and a garage trip.
//
// CAVEAT, and it is the whole story on this board: the deployed Feather is
// flashed with tinyuf2-partitions-4MB.csv, which has NO coredump partition.
// The libs enable coredump-to-flash, but there is nowhere to write it, so no
// dump has ever been stored here. supported() reports that instead of
// pretending, and every entry point refuses to call the IDF API without the
// partition -- doing so in setup() before WiFi is what took the board off the
// network on 1.14.31 and left the boot-health rollback as the only way home.
//
// Getting real core dumps on this unit means adding the partition, which
// rewrites the partition table and therefore needs a USB flash; OTA cannot do
// it. Until someone does that, the RTC breadcrumb ring is the crash evidence
// that actually works here.

#include <stddef.h>
#include <stdint.h>

namespace coredump {

/** Does this board even have a coredump partition? False on the deployed unit. */
bool supported();

/** Is a valid core dump sitting in flash? Always false without supported(). */
bool present();

/**
 * One line naming what died, into `out`.
 *
 * "panic in loopTask: LoadProhibited at 0x4008a1b2 (addr 0x00000000)"
 * Empty string when there is no dump.
 */
void one_line(char* out, size_t cap);

/**
 * The full summary as JSON: task, cause, PC, faulting address, backtrace PCs,
 * and the SHA of the build that crashed (so a dump from an older image is
 * recognisable rather than decoded against the wrong ELF).
 */
size_t to_json(char* out, size_t cap);

/** Byte range of the raw dump in flash, for streaming it out for espcoredump.py. */
bool image_range(size_t* addr, size_t* size);

/** Read `len` bytes of the raw dump at `off` into `buf`. */
bool read_image(size_t off, uint8_t* buf, size_t len);

/** Discard the stored dump. */
bool erase();

/**
 * Log one_line() to the flight recorder, once, early in boot.
 *
 * Called from setup() after crashlog::examine_boot so the tape reads
 * "boot ... cause=panic" immediately followed by what the panic actually was.
 */
void log_at_boot();

}  // namespace coredump
