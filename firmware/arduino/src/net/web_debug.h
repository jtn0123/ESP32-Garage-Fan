#pragma once
// Bench diagnostics behind /api/*: the raw SD handshake probe, the raw fuel
// gauge register dump, and the I2C scan. Kept apart from web.cpp because
// none of this is product surface -- it exists for the workbench, reads
// hardware directly, and should be easy to compile out someday.

#include <WebServer.h>

namespace web_debug {

/**
 * `token` is the same OTA/update token web.cpp guards /api/restart and
 * /api/sdformat with. Every diagnostic here requires it: /api/battdebug
 * writes fuel-gauge registers and /api/sdtest tears down the SD/SPI buses,
 * so reaching the page must not be enough to poke the hardware.
 */
void register_routes(WebServer& http, const char* token);

}  // namespace web_debug
