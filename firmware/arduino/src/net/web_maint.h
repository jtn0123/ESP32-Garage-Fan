#pragma once
// The maintenance and forensics routes: restart, SD format/purge, the crash
// dump trio, the event logs and the panel mirror. Split from web.cpp when it
// passed 1100 lines. Everything here is either token-guarded, destructive,
// or exists to answer "what happened" after the fact; none of it is the
// second-by-second product surface.

#include <WebServer.h>

namespace web_maint {

/**
 * `token` is the OTA/update token, shared for the same reason web_debug
 * shares it: restart, sdformat, sdpurge and the crash reads must not be
 * reachable by merely finding the page. The pointer aliases web.cpp's live
 * token buffer, so a runtime token change applies here without re-plumbing.
 */
void register_routes(WebServer& http, const char* token);

}  // namespace web_maint
