// See creds.h. One NVS blob is the commit boundary for a credential change.
#include "net/creds.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "net/numeric_arg.h"
#include "system/eventlog.h"

namespace creds {
namespace {
constexpr uint32_t kFormat = 0x43524431;  // CRD1; change the key for a future format
constexpr const char* kKey = "creds_v1";
struct Values {
  uint32_t format;
  uint16_t port;
  char ssid[kSsidCap];
  char pass[kPassCap];
  char host[kHostCap];
  char user[kUserCap];
  char mpass[kPassCap];
  char lat[kCoordCap];
  char lon[kCoordCap];
};
struct Field {
  const char* arg;
  size_t offset;
  size_t cap;
};
const Field kFields[] = {
    {"ssid", offsetof(Values, ssid), kSsidCap},
    {"pass", offsetof(Values, pass), kPassCap},
    {"mqtt_host", offsetof(Values, host), kHostCap},
    {"mqtt_user", offsetof(Values, user), kUserCap},
    {"mqtt_pass", offsetof(Values, mpass), kPassCap},
    {"lat", offsetof(Values, lat), kCoordCap},
    {"lon", offsetof(Values, lon), kCoordCap},
};
Values g_values{};
Preferences* g_prefs = nullptr;
char* buffer(Values& v, const Field& f) { return reinterpret_cast<char*>(&v) + f.offset; }
void put(char* dst, size_t cap, const char* src) { snprintf(dst, cap, "%s", src ? src : ""); }
bool persist(const Values& v) {
  return g_prefs && g_prefs->putBytes(kKey, &v, sizeof(v)) == sizeof(v);
}
bool valid(Values& v) {
  if (v.format != kFormat || v.port == 0)
    return false;
  for (const Field& f : kFields)
    if (!memchr(buffer(v, f), '\0', f.cap))
      return false;
  return true;
}
}  // namespace

void restore(Preferences* prefs) {
  g_prefs = prefs;
  g_values = {};
  g_values.format = kFormat;
  g_values.port = MQTT_PORT;
  put(g_values.ssid, kSsidCap, WIFI_SSID);
  put(g_values.pass, kPassCap, WIFI_PASS);
  put(g_values.host, kHostCap, MQTT_HOST);
  put(g_values.user, kUserCap, MQTT_USER);
  put(g_values.mpass, kPassCap, MQTT_PASS);
  put(g_values.lat, kCoordCap, WEATHER_LAT);
  put(g_values.lon, kCoordCap, WEATHER_LON);
  if (!prefs)
    return;
  const size_t stored_size = prefs->getBytesLength(kKey);
  if (stored_size != 0) {
    Values stored{};
    if (stored_size == sizeof(stored) &&
        prefs->getBytes(kKey, &stored, sizeof(stored)) == sizeof(stored) && valid(stored)) {
      g_values = stored;
    } else {
      // Never silently roll back to stale legacy credentials after migration.
      g_values = {};
      g_values.format = kFormat;
      g_values.port = 1883;
      eventlog::log("creds", "invalid credential snapshot; provisioning required");
    }
    return;
  }
  if (prefs->getString("ssid", "").length() != 0) {
    // Upgrade legacy stores without changing the credentials. Old keys remain
    // for rollback images; all subsequent changes use only the atomic blob.
    for (const Field& f : kFields)
      put(buffer(g_values, f), f.cap, prefs->getString(f.arg, buffer(g_values, f)).c_str());
    g_values.port = prefs->getUShort("mqtt_port", g_values.port);
  }
  if (g_values.ssid[0] && !persist(g_values))
    eventlog::log("creds", "credential seed/migration could not be saved");
}

const char* wifi_ssid() { return g_values.ssid; }
const char* wifi_pass() { return g_values.pass; }
const char* mqtt_host() { return g_values.host; }
uint16_t mqtt_port() { return g_values.port; }
const char* mqtt_user() { return g_values.user; }
const char* mqtt_pass() { return g_values.mpass; }
const char* weather_lat() { return g_values.lat; }
const char* weather_lon() { return g_values.lon; }
bool provisioned() { return g_values.ssid[0] != '\0'; }

Result apply_fields(const Input* fields, size_t count, const char** bad) {
  Values candidate = g_values;
  for (size_t i = 0; i < count; ++i) {
    const Input& input = fields[i];
    if (bad)
      *bad = input.arg;
    if (!input.arg || !input.value)
      return Result::Invalid;
    if (strcmp(input.arg, "mqtt_port") == 0) {
      long port = 0;
      if (!numeric_arg::integer(input.value, 1, 65535, &port))
        return Result::Invalid;
      candidate.port = static_cast<uint16_t>(port);
      continue;
    }
    bool found = false;
    for (const Field& f : kFields) {
      if (strcmp(input.arg, f.arg) != 0)
        continue;
      found = true;
      const size_t n = strlen(input.value);
      if (n >= f.cap || (n == 0 && strcmp(f.arg, "ssid") == 0))
        return Result::Invalid;
      put(buffer(candidate, f), f.cap, input.value);
      break;
    }
    if (!found)
      return Result::Invalid;
  }
  if (!count)
    return Result::Invalid;
  if (!persist(candidate))
    return Result::StorageFailure;
  g_values = candidate;
  return Result::Ok;
}
}  // namespace creds
