#pragma once
// Bench diagnostics behind /api/*: the raw SD handshake probe, the raw fuel
// gauge register dump, and the I2C scan. Kept apart from web.cpp because
// none of this is product surface -- it exists for the workbench, reads
// hardware directly, and should be easy to compile out someday.

#include <WebServer.h>

namespace web_debug {

void register_routes(WebServer& http);

}  // namespace web_debug
