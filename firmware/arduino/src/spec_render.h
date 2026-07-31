#pragma once
// Copyright 2026 Justin
//
// Pure render-path string helpers for the ui_spec op executor. No Arduino
// types on purpose: main.cpp consumes these on-device and the
// native_spec_render env tests them on the host, so template-expansion
// regressions surface in CI instead of as "--" on the glass.

#include <stddef.h>
#include <string.h>

// Resolves one {placeholder} key into out (always NUL-terminated). The device
// resolver reads live sensor/net state; tests use a fixed table.
typedef void (*SpecFieldResolver)(const char* key, char* out, size_t out_n);

// Expand every {placeholder} in templ into out. Literal text copies through;
// a '{' with no closing '}' copies literally, matching the String-based
// expander this replaced. Truncates at out_n-1 and always NUL-terminates.
inline void spec_expand_template_into(const char* templ, SpecFieldResolver resolve, char* out,
                                      size_t out_n) {
  if (!out || out_n == 0)
    return;
  size_t w = 0;
  out[0] = '\0';
  if (!templ)
    return;
  const char* p = templ;
  while (*p && w + 1 < out_n) {
    if (*p == '{') {
      const char* rb = strchr(p + 1, '}');
      if (!rb) {
        while (*p && w + 1 < out_n) out[w++] = *p++;
        break;
      }
      char key[40];
      size_t klen = static_cast<size_t>(rb - (p + 1));
      if (klen >= sizeof(key))
        klen = sizeof(key) - 1;
      memcpy(key, p + 1, klen);
      key[klen] = '\0';
      char val[48];
      val[0] = '\0';
      resolve(key, val, sizeof(val));
      for (const char* v = val; *v && w + 1 < out_n; ++v) out[w++] = *v;
      p = rb + 1;
    } else {
      out[w++] = *p++;
    }
  }
  out[w] = '\0';
}

// Short user-facing label for a raw weather condition string. Keyword order
// mirrors the web sim's shortConditionLabel so both surfaces render the same
// footer text for the same condition.
inline const char* spec_short_condition(const char* weather) {
  char s[64];
  size_t i = 0;
  for (; weather && weather[i] && i + 1 < sizeof(s); ++i) {
    char c = weather[i];
    s[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  }
  s[i] = '\0';
  auto has = [&](const char* k) { return strstr(s, k) != nullptr; };
  if (has("night") || has("moon"))
    return "Night";
  if (has("part"))
    return "Partly";
  if (has("cloud") || has("overcast"))
    return "Cloudy";
  if (has("storm") || has("thunder") || has("lightning"))
    return "Storm";
  if (has("rain") || has("pour") || has("shower") || has("drizzle"))
    return "Rain";
  if (has("hail"))
    return "Hail";
  if (has("snow") || has("sleet"))
    return "Snow";
  if (has("fog") || has("mist") || has("haze"))
    return "Fog";
  if (has("wind"))
    return "Wind";
  return "Sunny";
}
