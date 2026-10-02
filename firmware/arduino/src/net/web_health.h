#pragma once

#include <WebServer.h>

namespace web_health {

/** Read-only GET /health for Uptime Kuma and other HTTP monitors. */
void register_routes(WebServer& http);

}  // namespace web_health
