#pragma once
#include <Preferences.h>

#include "net/token_policy.h"

namespace admin_token {
// A credential-free release must retain the administrator token established
// by the first local image. Never overwrite an existing private NVS token.
inline String restore(Preferences* prefs, const char* compiled) {
  const String stored = prefs ? prefs->getString("token", "") : String("");
  if (token_policy::usable(stored.c_str()))
    return stored;
  if (!token_policy::usable(compiled))
    return String("");
  const String candidate(compiled);
  if (prefs && prefs->putString("token", candidate) != candidate.length())
    return String("");  // failed persistence must not pretend the token is safe
  return candidate;
}
}  // namespace admin_token
