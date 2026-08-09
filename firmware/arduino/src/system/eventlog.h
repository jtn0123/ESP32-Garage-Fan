#pragma once
// The flight recorder: one-line, timestamped events (WiFi drops with reason,
// MQTT edges, boots with cause of death, OTA, fan commands, 5-min health
// stats) kept in a RAM ring and drained to /events.log on the SD card. Built
// to answer exactly one question after a random disconnect: what did the
// board see in the minutes before it went dark?
//
// Line format: "<epoch> <uptime_s> <tag> <message>" -- epoch is 0 before SNTP
// syncs, so uptime is the field that always orders a single boot's story.
// The ring serves /api/events even with no card; the SD file is the record
// that survives reboots (RAM obviously does not).

#include <stdint.h>

namespace eventlog {

/** Append one event. printf-style; the rendered line is capped at ~100 chars. */
void log(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

/**
 * Drain a few not-yet-written lines to the SD card. Loop cadence; cheap when
 * there is nothing pending or no card. Events logged before the card mounts
 * are flushed once it does -- the ring keeps a ledger of what reached the file.
 */
void flush_tick();

/** Lines currently retained in RAM. */
uint16_t count();

/** Copy retained line i (0 = oldest) into dst. Safe against the event task. */
void copy_line(uint16_t i, char* dst, uint32_t cap);

/** Lines overwritten before they could reach the SD file. */
uint32_t lost();

}  // namespace eventlog
