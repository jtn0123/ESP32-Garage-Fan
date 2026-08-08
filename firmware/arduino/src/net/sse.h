#pragma once
// Server-sent events on port 8081: live state pushes to every open console.
// Separate WiFiServer rather than a WebServer route because the framework
// cannot hold a response open; see the transport rules in http_tx.h for why
// a slow peer must never be waited on.

#include <cstddef>

namespace sse {

/** Provider of the current state JSON -- registered by the web module. */
using StateSource = void (*)(char* out, size_t cap);
void set_state_source(StateSource src);

void start();   // begin listening (call once WiFi is up)
void accept();  // poll for new subscribers; greets each with current state
void push();    // broadcast current state to every connected peer

}  // namespace sse
