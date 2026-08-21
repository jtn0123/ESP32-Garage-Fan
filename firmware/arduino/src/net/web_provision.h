#pragma once
// Copyright 2026 Justin
//
// POST /api/provision -- the settings page's way to change the credentials
// creds.h holds in NVS. Token-guarded (the OTA token) and origin-guarded like
// every other write; applies the given fields and reboots so the WiFi and
// MQTT stacks start on the new values. Secrets travel in the POST body only
// and are never echoed or logged.
#include <WebServer.h>

namespace web_provision {
void register_routes(WebServer& http, const char* token);
}  // namespace web_provision
