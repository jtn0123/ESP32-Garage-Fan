#pragma once
// The two gates every state-changing route stands behind, shared by web.cpp
// and its route modules (web_history, web_maint, web_debug, web_ota). Header-
// only so a gate check is exactly the same code in every translation unit --
// three drifting copies of a CSRF guard is how one of them quietly rots.

#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "net/origin_check.h"
#include "system/eventlog.h"

namespace web_gate {

// An empty configured token must never authorize anything: with token ""
// and no ?token= argument, arg() returns "" and a bare equality check would
// wave the request through. Unreachable with today's defaults, but this is
// the line every privileged route stands behind, so it does not get to rely
// on the defaults staying friendly.
inline bool token_ok(const char* token, const String& presented) {
  return token[0] != '\0' && presented == token;
}

/** token_ok against ?token=, with the 403 already sent. True means go on. */
inline bool guard_token(WebServer& http, const char* token) {
  if (token_ok(token, http.arg("token")))
    return true;
  http.send(403, "application/json", "{\"error\":\"bad token\"}");
  return false;
}

/**
 * Reject a state-changing request that a foreign page told the browser to make.
 *
 * POST-only is NOT sufficient on its own, which is the correction that produced
 * this function: an HTML form can be submitted cross-origin with method=POST
 * and no preflight, so `<form action="http://garage-fan.local/api/set?speed=0">`
 * with a scripted submit() reaches the handler exactly as a GET `<img src>`
 * used to. The response stays unreadable cross-origin, but the write lands, and
 * the write is the whole attack.
 *
 * A request with NO Origin header is allowed: that is curl, the deploy script,
 * and every other non-browser client. Browsers always attach Origin to a
 * cross-origin POST, so this closes the browser-driven path without breaking
 * the bench tooling. These endpoints remain unauthenticated on the LAN by
 * deliberate choice; this is about who gets to speak for the operator.
 *
 * Requires web::begin()'s collectHeaders("Origin") -- WebServer discards every
 * header it was not told to keep, and a discarded Origin looks exactly like a
 * non-browser caller.
 */
inline bool origin_ok(WebServer& http) {
  if (!http.hasHeader("Origin"))
    return true;  // non-browser caller
  // The comparison itself is net::origin_is_self, which the host tests cover
  // (native_origin_check) -- including the lookalike hosts a prefix match
  // would have let through.
  // Present-but-empty is NOT absent. origin_is_self treats "" as "no browser
  // sent one", which is right for a missing header and wrong here: the client
  // did send it. sse.cpp already drew this distinction; both now agree.
  const String origin = http.header("Origin");
  return origin.length() > 0 &&
         net::origin_is_self(origin.c_str(), WiFi.localIP().toString().c_str(), FAN_HOSTNAME);
}

/** origin_ok() with the 403 already sent. True means "keep going". */
inline bool guard_origin(WebServer& http) {
  if (origin_ok(http))
    return true;
  eventlog::log("web", "rejected cross-origin write from %s", http.header("Origin").c_str());
  http.send(403, "application/json", "{\"error\":\"cross-origin request refused\"}");
  return false;
}

}  // namespace web_gate
