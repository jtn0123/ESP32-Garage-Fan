// See creds.h.
#include "net/creds.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "system/eventlog.h"

namespace creds {
namespace {

Preferences* g_prefs = nullptr;
char g_ssid[kSsidCap] = "";
char g_wpass[kPassCap] = "";
char g_host[kHostCap] = "";
uint16_t g_port = 1883;
char g_user[kUserCap] = "";
char g_mpass[kPassCap] = "";
char g_lat[kCoordCap] = "";
char g_lon[kCoordCap] = "";

struct Field {
  const char* arg;  // /api/provision argument and NVS key (short, <= 15 chars)
  char* buf;
  size_t cap;
};

// One table drives restore, seed and set: a field added here is a field
// persisted, served and settable, nowhere else to forget.
const Field kFields[] = {
    {"ssid", g_ssid, sizeof(g_ssid)},        {"pass", g_wpass, sizeof(g_wpass)},
    {"mqtt_host", g_host, sizeof(g_host)},   {"mqtt_user", g_user, sizeof(g_user)},
    {"mqtt_pass", g_mpass, sizeof(g_mpass)}, {"lat", g_lat, sizeof(g_lat)},
    {"lon", g_lon, sizeof(g_lon)},
};

void put(char* dst, size_t cap, const char* src) { snprintf(dst, cap, "%s", src ? src : ""); }

// What generated_config.h baked in. Empty on a CI/release build.
void load_compiled() {
  put(g_ssid, sizeof(g_ssid), WIFI_SSID);
  put(g_wpass, sizeof(g_wpass), WIFI_PASS);
  put(g_host, sizeof(g_host), MQTT_HOST);
  g_port = MQTT_PORT;
  put(g_user, sizeof(g_user), MQTT_USER);
  put(g_mpass, sizeof(g_mpass), MQTT_PASS);
  put(g_lat, sizeof(g_lat), WEATHER_LAT);
  put(g_lon, sizeof(g_lon), WEATHER_LON);
}

void persist_all() {
  if (!g_prefs)
    return;
  for (const Field& f : kFields) g_prefs->putString(f.arg, f.buf);
  g_prefs->putUShort("mqtt_port", g_port);
}

}  // namespace

void restore(Preferences* prefs) {
  g_prefs = prefs;
  load_compiled();
  if (!prefs)
    return;
  if (prefs->getString("ssid", "").length() == 0) {
    // Nothing provisioned yet. A build carrying credentials seeds the store
    // so the NEXT image -- a credential-free release -- still gets online.
    // A credential-free build on a blank board has nothing to seed; it stays
    // on the compiled (empty) values and the board is unreachable until a
    // credentialed image or a future provisioning path reaches it.
    if (g_ssid[0] != '\0') {
      persist_all();
      eventlog::log("creds", "seeded nvs from the compiled image (ssid=%s)", g_ssid);
    }
    return;
  }
  for (const Field& f : kFields) put(f.buf, f.cap, prefs->getString(f.arg, f.buf).c_str());
  g_port = prefs->getUShort("mqtt_port", g_port);
}

const char* wifi_ssid() { return g_ssid; }
const char* wifi_pass() { return g_wpass; }
const char* mqtt_host() { return g_host; }
uint16_t mqtt_port() { return g_port; }
const char* mqtt_user() { return g_user; }
const char* mqtt_pass() { return g_mpass; }
const char* weather_lat() { return g_lat; }
const char* weather_lon() { return g_lon; }
bool provisioned() { return g_ssid[0] != '\0'; }

bool set_field(const char* arg, const char* value) {
  if (!arg || !value)
    return false;
  if (strcmp(arg, "mqtt_port") == 0) {
    char* end = nullptr;
    const long p = strtol(value, &end, 10);
    if (end == value || *end != '\0' || p < 1 || p > 65535)
      return false;
    g_port = static_cast<uint16_t>(p);
    if (g_prefs)
      g_prefs->putUShort("mqtt_port", g_port);
    return true;
  }
  for (const Field& f : kFields) {
    if (strcmp(arg, f.arg) != 0)
      continue;
    const size_t n = strlen(value);
    if (n >= f.cap)
      return false;  // over-long: never truncate a credential silently
    if (n == 0 && strcmp(arg, "ssid") == 0)
      return false;  // an empty SSID is "unprovisioned", not a setting
    put(f.buf, f.cap, value);
    if (g_prefs)
      g_prefs->putString(f.arg, f.buf);
    return true;
  }
  return false;
}

}  // namespace creds
