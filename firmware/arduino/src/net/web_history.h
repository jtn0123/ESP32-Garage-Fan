#pragma once
// The read-side data routes: /api/history (chart series), /download.csv
// (the raw export) and /api/stats (odometer + temperature extremes). Split
// from web.cpp when it passed 1100 lines; these three share the series
// writers and the PSRAM chart scratch and nothing else in the web layer
// touches either.

#include <WebServer.h>

namespace web_history {

void register_routes(WebServer& http);

}  // namespace web_history
