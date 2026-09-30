#pragma once
#include <Preferences.h>
#include <WebServer.h>

namespace web_controls {
using StateReply = void (*)();
void register_routes(WebServer& http, Preferences* prefs, char (&token)[40], StateReply state,
                     StateReply push);
}  // namespace web_controls
