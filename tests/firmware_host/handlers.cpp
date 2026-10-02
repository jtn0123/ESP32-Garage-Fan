// Run production handlers and control logic against fallible transport/storage/drivers.
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>
#include "net/creds.h"
#include "net/admin_token.h"
#include "net/web_controls.h"
#include "net/web_provision.h"
#include "fan/control.h"
#include "system/odometer.h"
static WebServer http;
static int announces = 0;
static void state() { http.send(200, "application/json", "{}"); }
static void push() { ++announces; }
static void notify(int, bool) { ++announces; }
int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string test = argv[1];
  Preferences prefs;
  char token[40] = "example-update-token";
  creds::restore(&prefs);
  if (test == "actuator-attach")
    attach_ok = false;
  fan::restore(&prefs);
  fan::set_notify(notify);
  web_controls::register_routes(http, &prefs, token, state, push);
  web_provision::register_routes(http, token);
  http.headers["X-Fan-Token"] = token;
  // Older implementations used a query token. Including it here lets the
  // credential/actuator regressions independently reproduce on the old code.
  http.args["token"] = token;
  if (test == "credential-length") {
    http.args["ssid"] = "new-net";
    http.args["mqtt_pass"] = std::string(65, 'x');
    http.request("/api/provision");
    assert(http.status == 400);
    assert(std::strcmp(creds::wifi_ssid(), "old-net") == 0);
    creds::restore(&prefs);
    assert(std::strcmp(creds::wifi_ssid(), "old-net") == 0);
    assert(!restarted);
  } else if (test == "credential-port") {
    http.args["ssid"] = "new-net";
    http.args["mqtt_port"] = "1883junk";
    http.request("/api/provision");
    assert(http.status == 400);
    assert(std::strcmp(creds::wifi_ssid(), "old-net") == 0);
    assert(!restarted);
  } else if (test == "credential-storage") {
    const auto old = prefs.blobs;
    prefs.fail_write = true;
    http.args["ssid"] = "new-net";
    http.request("/api/provision");
    assert(http.status == 503);
    assert(prefs.blobs == old);
    assert(std::strcmp(creds::wifi_ssid(), "old-net") == 0);
    assert(!restarted);
  } else if (test == "credential-success") {
    http.args["ssid"] = "new-net";
    http.args["pass"] = " wifi ";
    http.args["mqtt_pass"] = " mqtt ";
    http.args["mqtt_port"] = "65535";
    http.request("/api/provision");
    assert(http.status == 200 && restarted);
    creds::restore(&prefs);
    assert(std::strcmp(creds::wifi_ssid(), "new-net") == 0);
    assert(std::strcmp(creds::wifi_pass(), " wifi ") == 0);
    assert(std::strcmp(creds::mqtt_pass(), " mqtt ") == 0);
    assert(creds::mqtt_port() == 65535);
  } else if (test == "credential-migration") {
    Preferences legacy;
    legacy.strings["ssid"] = "legacy-net";
    legacy.strings["pass"] = "legacy-pass";
    legacy.numbers["mqtt_port"] = 1884;
    creds::restore(&legacy);
    assert(std::strcmp(creds::wifi_ssid(), "legacy-net") == 0);
    assert(!legacy.blobs["creds_v1"].empty());
    legacy.strings["ssid"] = "stale-net";
    creds::restore(&legacy);
    assert(std::strcmp(creds::wifi_ssid(), "legacy-net") == 0);
  } else if (test == "numeric-speed") {
    for (const char* value : {"junk", "", "3junk", "2.5", "999999999999999999999999", "-1", "13"}) {
      http.args["speed"] = value;
      http.request("/api/set");
      assert(http.status == 400);
      assert(fan::speed() == 0 && announces == 0);
    }
    http.args["speed"] = "12";
    http.request("/api/set");
    assert(http.status == 200 && fan::speed() == 12);
  } else if (test == "numeric-config") {
    for (const char* value : {"nan", "inf", "2.5junk", "", "1e999"}) {
      http.args["max"] = "12";
      http.args["onf"] = value;
      http.request("/api/config");
      assert(http.status == 400);
      assert(fan::auto_max() == 9 && announces == 0);
    }
    http.args.erase("onf");
    http.args["auto"] = "2";
    http.request("/api/config");
    assert(http.status == 400 && fan::auto_max() == 9);
  } else if (test == "limit-config") {
    // The winter limits: both switches and both temperatures land, persist,
    // and a bad value in any one of them changes nothing at all.
    assert(!fan::floor_on() && !fan::start_on());
    http.args["flooron"] = "1";
    http.args["floorf"] = "62.5";
    http.args["starton"] = "1";
    http.args["startf"] = "75";
    http.request("/api/config");
    assert(http.status == 200);
    assert(fan::floor_on() && fan::floor_f() == 62.5f);
    assert(fan::start_on() && fan::start_f() == 75.0f);
    fan::restore(&prefs);  // what a reboot would read back
    assert(fan::floor_on() && fan::floor_f() == 62.5f && fan::start_on());
    for (const char* bad : {"2", "on", ""}) {
      http.args.clear();
      http.headers["X-Fan-Token"] = token;
      http.args["floorf"] = "70";
      http.args["flooron"] = bad;
      http.request("/api/config");
      assert(http.status == 400 && fan::floor_on() && fan::floor_f() == 62.5f);
    }
    for (const char* bad : {"31", "121", "nan"}) {
      http.args.clear();
      http.headers["X-Fan-Token"] = token;
      http.args["flooron"] = "0";
      http.args["startf"] = bad;
      http.request("/api/config");
      assert(http.status == 400 && fan::floor_on() && fan::start_f() == 75.0f);
    }
    // Manual mode has no thermostat, so it claims no limit.
    fan::set_auto(false);
    assert(fan::limit() == nullptr);
  } else if (test == "numeric-raw") {
    for (const char* value : {"garbage", "10junk", "1.5", "101", ""}) {
      http.args["high_pct"] = value;
      http.request("/api/raw");
      assert(http.status == 400 && fan::speed() == 0);
    }
  } else if (test == "actuator-write") {
    fan::apply(9, "http", false);
    fan::set_auto(true);
    const int previous_announces = announces;
    const auto saved = prefs.numbers;
    const uint16_t high = fan::commanded_high_us();
    write_ok = false;
    http.args["speed"] = "10";
    http.request("/api/set");
    assert(http.status == 503);
    assert(fan::speed() == 9 && fan::auto_on());
    assert(fan::commanded_high_us() == high);
    assert(announces == previous_announces && prefs.numbers == saved);
    http.args["high_pct"] = "100";
    http.request("/api/raw");
    assert(http.status == 503 && fan::speed() == 9);
    write_ok = true;
    http.request("/api/set");
    assert(http.status == 200 && fan::speed() == 10 && !fan::auto_on());
  } else if (test == "actuator-attach") {
    assert(fan::speed() == -2);
    http.args["speed"] = "12";
    http.request("/api/set");
    assert(http.status == 503 && fan::speed() == -2 && announces == 0);
    attach_ok = true;
    http.request("/api/set");
    assert(http.status == 200 && fan::speed() == 12);
  } else if (test == "query-auth") {
    http.headers.clear();
    http.args["ssid"] = "new-net";
    http.request("/api/provision");
    assert(http.status == 403 && !restarted);
  } else if (test == "public-token") {
    snprintf(token, sizeof(token), "%s", "iliving-ota");
    http.headers["X-Fan-Token"] = token;
    http.args["token"] = token;
    http.args["ssid"] = "new-net";
    http.request("/api/provision");
    assert(http.status == 403 && !restarted);
  } else if (test == "origin") {
    http.headers["Origin"] = "http://evil.example";
    http.args["speed"] = "12";
    http.request("/api/set");
    assert(http.status == 403 && fan::speed() == 0);
  } else if (test == "token-rotation") {
    http.args["newtoken"] = "next-test-only-token";
    http.request("/api/config");
    assert(http.status == 200 && std::strcmp(token, "next-test-only-token") == 0);
    assert(prefs.strings["token"] == token);
    http.args["newtoken"] = "another-test-only-token";
    http.request("/api/config");
    assert(http.status == 403);
  } else if (test == "token-bootstrap") {
    prefs.strings["token"] = "iliving-ota";
    assert(admin_token::restore(&prefs, "private-test-only-token") == "private-test-only-token");
    assert(admin_token::restore(&prefs, "") == "private-test-only-token");
    assert(admin_token::restore(&prefs, "different-test-only-token") == "private-test-only-token");
  } else if (test == "token-bootstrap-failure") {
    prefs.fail_write = true;
    assert(admin_token::restore(&prefs, "private-test-only-token").empty());
    assert(admin_token::restore(&prefs, "iliving-ota").empty());
  } else if (test == "token-storage") {
    prefs.fail_write = true;
    http.args["newtoken"] = "next-test-only-token";
    http.args["max"] = "12";
    http.request("/api/config");
    assert(http.status == 503 && fan::auto_max() == 9);
    assert(std::strcmp(token, "example-update-token") == 0);
  } else {
    assert(false);
  }
}
