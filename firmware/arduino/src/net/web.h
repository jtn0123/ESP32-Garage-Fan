#pragma once
// The HTTP surface: the console page, every /api endpoint, OTA upload, and
// the state JSON that both /api/state and the SSE stream serve. This is the
// aggregation layer -- it reads every other module and owns none of them.

#include <Preferences.h>

#include <cstddef>

namespace web {

/** Restore the OTA token and register every route. Call once in setup(). */
void begin(Preferences* prefs);

void start();   // server.begin() once WiFi is up
void handle();  // WebServer poll; call every loop()

/** The /api/state payload; also the SSE frame body. */
void state_json(char* out, size_t cap);

/** Rebuild state and broadcast it over SSE. */
void push_state();

}  // namespace web
