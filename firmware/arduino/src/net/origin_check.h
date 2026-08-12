#pragma once
// Does an Origin header belong to this device? Separated so the host can test
// it, because it is the guard standing in front of every mutating route.
//
// POST-only is NOT sufficient on its own. A cross-origin HTML form submits with
// method=POST and no preflight, so `<form action="http://garage-fan.local/
// api/set?speed=0">` with a scripted submit() reaches the handler exactly as a
// GET `<img src>` used to. The response stays unreadable to the attacker, but
// the write lands, and the write was always the attack.
//
// A request with NO Origin is allowed: that is curl, the deploy script, and
// every other non-browser client. Browsers always attach Origin to a
// cross-origin POST, so this closes the browser-driven path without breaking
// the bench tooling. These endpoints remain unauthenticated on the LAN by
// deliberate choice; this is about who gets to speak for the operator.

#include <stdio.h>
#include <string.h>

namespace net {

/**
 * Compare `origin` against this device's own origins.
 *
 * @param origin   the header value, or nullptr/"" when absent
 * @param ip       dotted-quad, e.g. "192.0.2.187"
 * @param hostname mDNS name without the suffix, e.g. "garage-fan"
 *
 * Absent means "not a browser" and is allowed. Everything else must match one
 * of http://<ip>, http://<hostname>, http://<hostname>.local exactly --
 * exactly, so that a lookalike host cannot pass on a prefix.
 */
inline bool origin_is_self(const char* origin, const char* ip, const char* hostname) {
  if (!origin || !*origin)
    return true;  // non-browser caller
  // "null" is what a sandboxed iframe or a file:// page sends. It is not this
  // device and must never be treated as absent.
  if (strcasecmp(origin, "null") == 0)
    return false;

  // An EMPTY identity must not produce a matchable origin. With ip == "",
  // "http://%s" collapses to the bare "http://" -- and a caller sending
  // exactly that header would then be accepted as this device. Not reachable
  // today (WiFi.localIP() renders "0.0.0.0" when down rather than empty), but
  // a security guard should not rest on a formatting accident somewhere else.
  const bool has_ip = ip && *ip;
  const bool has_host = hostname && *hostname;

  char self[3][80];
  int n = 0;
  if (has_ip)
    snprintf(self[n++], sizeof(self[0]), "http://%s", ip);
  if (has_host) {
    snprintf(self[n++], sizeof(self[0]), "http://%s", hostname);
    snprintf(self[n++], sizeof(self[0]), "http://%s.local", hostname);
  }
  for (int i = 0; i < n; i++) {
    // Whole-string, case-insensitive. A prefix comparison would accept
    // http://garage-fan.local.evil.example, which is a different site.
    if (strcasecmp(origin, self[i]) == 0)
      return true;
  }
  return false;
}

}  // namespace net
