#pragma once
// The OTA upload endpoint: POST /update?token=..., streamed into the inactive
// A/B slot. Split from web.cpp so the route file stays under the 500-line
// ceiling; the flow itself (authorize at UPLOAD_FILE_START, write, reboot on
// success, ota_rollback confirms only after the broker answers) is unchanged.

#include <WebServer.h>

namespace web_ota {

/**
 * Register POST /update on `http`. `token` is web's live token buffer --
 * compared at request time, so a runtime token change applies here too.
 */
void register_routes(WebServer& http, const char* token);

}  // namespace web_ota
