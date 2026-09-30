#pragma once
#include <cstring>
namespace token_policy {
// Public defaults and setup placeholders never grant administrator access.
inline bool usable(const char* token) {
  return token && std::strlen(token) >= 6 && std::strlen(token) < 39 &&
         std::strchr(token, '\r') == nullptr && std::strchr(token, '\n') == nullptr &&
         std::strcmp(token, "iliving-ota") != 0 &&
         std::strcmp(token, "pick_a_long_random_token") != 0;
}
}  // namespace token_policy
