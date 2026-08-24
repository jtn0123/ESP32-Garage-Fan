#pragma once
// Copyright 2026 Justin
//
// Runtime credentials: WiFi, MQTT and the weather coordinates, held in NVS
// and owned here, so a published release image -- which is built on CI from
// a clean checkout and therefore carries EMPTY compiled-in credentials -- is
// installable on a provisioned board. Before this module, the only image
// that could join the network was one built on a machine holding .env, and
// "update the fan" meant a laptop, a build and a file picker (2026-08-20).
//
// Precedence: a value stored in NVS wins. The compiled-in defaults from
// generated_config.h are used only to SEED NVS once, the first time a build
// that knows about this module boots on a board whose NVS has no SSID yet --
// which is how the already-deployed board provisions itself without anyone
// typing a password. After that, the settings page (POST /api/provision,
// token-guarded) is the way credentials change, and a reflash of any image,
// credentialed or not, leaves them alone.
#include <Preferences.h>

#include <cstdint>

namespace creds {

// Sizes are the protocol maxima plus the terminator.
constexpr size_t kSsidCap = 33;  // 802.11 SSID <= 32 bytes
constexpr size_t kPassCap = 65;  // WPA2 passphrase <= 63, plus room
constexpr size_t kHostCap = 65;
constexpr size_t kUserCap = 65;
constexpr size_t kCoordCap = 17;  // "-123.4567" and then some

// Load from NVS; seed NVS from the compiled defaults when it holds no SSID.
// Must run before wifi_link::begin() and mqtt_link::init().
void restore(Preferences* prefs);

const char* wifi_ssid();
const char* wifi_pass();
const char* mqtt_host();
uint16_t mqtt_port();
const char* mqtt_user();
const char* mqtt_pass();
const char* weather_lat();
const char* weather_lon();

// True once an SSID is known from either source.
bool provisioned();

// Fields /api/provision may set, by their wire argument names. Returns false
// (and stores nothing) for an unknown field, an over-long value, an empty
// SSID or an out-of-range port. Persists to NVS and updates the live value;
// the network stacks pick the change up on the next boot.
bool set_field(const char* arg, const char* value);

}  // namespace creds
