#!/usr/bin/env bash
# Build the fan controller firmware, push it over the air, and VERIFY it came
# back confirmed.
#   scripts/deploy.sh [device-host]           # default garage-fan.local
#   FAN_OTA_TOKEN=... scripts/deploy.sh       # non-default OTA token
#   DEPLOY_VERIFY_TIMEOUT=600 scripts/deploy.sh
#
# This replaces an espota.py-based script inherited from ESP32-Temp-Sensor. That
# one could never have worked here: the fan firmware links no ArduinoOTA, so
# nothing listens on port 3232, and its verify polled espsensor/+/availability
# and a homeassistant/sensor/..._temperature/config topic that this device never
# publishes. The fan's OTA is a plain multipart POST to /update?token=...
#
# The verify step exists because an upload "succeeding" only proves the bytes
# were written. The firmware writes to the inactive A/B slot and ota_rollback
# confirms an image only after it reaches the MQTT broker; an unconfirmed image
# is reverted after three failed boots. So the outcomes are: confirmed on the
# new version, rolled back onto the OLD version, or genuinely dark.
set -euo pipefail
HOST="${1:-garage-fan.local}"
ENV="${DEPLOY_ENV:-feather_esp32s2_fan_controller}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERIFY_TIMEOUT="${DEPLOY_VERIFY_TIMEOUT:-300}"
TOKEN="${FAN_OTA_TOKEN:-iliving-ota}"

pio run -d "$ROOT/firmware/arduino" -e "$ENV"

BIN="$ROOT/firmware/arduino/.pio/build/$ENV/firmware.bin"
[ -f "$BIN" ] || { echo "ABORT: no firmware.bin at $BIN"; exit 1; }

# The version this build carries. Read from the generated header rather than
# the VERSION file, because the header is what the compiler actually saw: if a
# stale build is sitting in .pio, this catches it instead of verifying against a
# number that was never flashed. Read before flashing so a failed verify can say
# which version we EXPECTED to see.
EXPECTED_FW=$(sed -n 's/^#define FW_VERSION "\(.*\)"$/\1/p' \
  "$ROOT/firmware/arduino/src/generated_config.h" 2>/dev/null | head -1)
if [ -z "$EXPECTED_FW" ]; then
  echo "ABORT: could not read FW_VERSION from generated_config.h."
  echo "Build first (make build) so the header exists. Without it the"
  echo "post-deploy verify would compare empty against empty and report"
  echo "VERIFIED for any device that answers."
  exit 1
fi

# Refuse to ship an image with no WiFi credentials. gen_device_header only
# WARNS on a missing WIFI_SSID (CI builds legitimately have none), and a build
# run in a shell without the repo .env sourced regenerates the header with
# empty credentials -- flashed once on 2026-08-01, which bricked the device's
# network until a USB reflash. A warning scrolled past in build output is not a
# gate; this is.
BUILT_SSID=$(sed -n 's/#define WIFI_SSID "\(.*\)"/\1/p' \
  "$ROOT/firmware/arduino/src/generated_config.h" 2>/dev/null | head -1)
if [ -z "$BUILT_SSID" ]; then
  echo "ABORT: the built image has an EMPTY WiFi SSID."
  echo "Source the repo .env (or set WIFI_SSID/WIFI_PASSWORD) and rebuild;"
  echo "flashing this image would leave the device unreachable over the network."
  exit 1
fi

echo "Uploading $(basename "$BIN") ($(wc -c <"$BIN" | tr -d ' ') bytes) to $HOST..."
code=$(curl -s -o /tmp/fan-ota-resp.txt -w '%{http_code}' -m 180 \
  -F "firmware=@$BIN" "http://$HOST/update?token=$TOKEN")
if [ "$code" != "200" ]; then
  echo "ABORT: /update returned HTTP $code"
  echo "  $(head -c 300 /tmp/fan-ota-resp.txt)"
  echo "  (401/403 means a wrong FAN_OTA_TOKEN)"
  exit 1
fi

echo "Uploaded; device rebooting - verifying it comes back confirmed..."

# --- verify -------------------------------------------------------------------
# /api/state carries both the running version and the A/B confirm flag, so one
# endpoint answers "is it up", "is it the new image", and "did it stick".
started=$SECONDS
deadline=$((SECONDS + VERIFY_TIMEOUT))
fw=""
while [ $SECONDS -lt $deadline ]; do
  state=$(curl -s -m 5 "http://$HOST/api/state" 2>/dev/null || true)
  if [ -n "$state" ]; then
    fw=$(echo "$state" | sed -n 's/.*"fw":"\([^"]*\)".*/\1/p')
    confirmed=$(echo "$state" | sed -n 's/.*"confirmed":\(true\|false\).*/\1/p')
    if [ "$fw" = "$EXPECTED_FW" ] && [ "$confirmed" = "true" ]; then
      echo "VERIFIED: $HOST online, running $fw, image confirmed ($((SECONDS - started))s)"
      exit 0
    elif [ "$fw" = "$EXPECTED_FW" ]; then
      echo "  running $fw but not yet confirmed (needs the broker), waiting..."
    elif [ -n "$fw" ]; then
      echo "  online but reporting $fw (want $EXPECTED_FW), waiting..."
    fi
  fi
  sleep 5
done

if [ "$fw" = "$EXPECTED_FW" ]; then
  echo "DEPLOY UNCONFIRMED: $HOST runs $EXPECTED_FW but never confirmed the image."
  echo "It could not reach the MQTT broker; boot-health will roll it back after"
  echo "three failed boots. Check the broker before power-cycling."
elif [ -n "$fw" ]; then
  echo "DEPLOY ROLLED BACK: $HOST is online but reports $fw, not $EXPECTED_FW."
  echo "The new image never reached the broker and boot-health reverted it."
else
  echo "DEPLOY UNVERIFIED: $HOST did not answer /api/state within ${VERIFY_TIMEOUT}s."
  echo "Note ESP32-S2 CDC drops serial prints unless a DTR-asserting terminal is"
  echo "attached, so a silent board is not proof of failure - try mDNS first."
fi
exit 1
