#!/usr/bin/env bash
# Build the always-on firmware, deploy it over the air, and VERIFY it came back.
#   scripts/deploy.sh [device-ip]
#   DEPLOY_ENV=feather_esp32s2_always_on_3c scripts/deploy.sh   # tri-color wing
#   DEPLOY_VERIFY_TIMEOUT=600 scripts/deploy.sh                 # patient verify
#
# Locates espota.py inside the installed arduino-esp32 platform package, which
# is otherwise a path nobody should have to remember. (The old advice of
# `pio run -t upload --project-option=...` no longer parses on pio 6.x.)
#
# The verify step exists because an espota "100% Done" only proves the bytes
# were written -- 2026-08-01 produced a device that flashed clean, rebooted, and
# never rejoined the network. The firmware now self-heals (link recovery) and
# rolls back never-confirmed images (boot health), so the outcomes here are:
# confirmed on the new version, recovered onto the OLD version (rollback), or
# genuinely dark (go get the cable).
set -euo pipefail
IP="${1:-10.27.27.187}"
ENV="${DEPLOY_ENV:-feather_esp32s2_always_on}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERIFY_TIMEOUT="${DEPLOY_VERIFY_TIMEOUT:-300}"

pio run -d "$ROOT/firmware/arduino" -e "$ENV"

ESPOTA=$(find ~/.platformio/packages -name espota.py -path "*arduinoespressif32*" | head -1)
[ -n "$ESPOTA" ] || { echo "espota.py not found - is the espressif32 platform installed?"; exit 1; }

# The version this build carries, for the post-deploy check. Read before
# flashing so a failed verify can say which version we EXPECTED to see.
EXPECTED_FW=$(sed -n 's/#define FW_VERSION "\(.*\)"/\1/p' \
  "$ROOT/firmware/arduino/src/generated_config.h" 2>/dev/null | head -1)

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

python3 "$ESPOTA" -i "$IP" -p 3232 \
  -f "$ROOT/firmware/arduino/.pio/build/$ENV/firmware.bin" -r
echo
echo "Deployed $ENV to $IP - device rebooting; verifying it comes back..."

# --- verify -------------------------------------------------------------------
# Credentials: the repo .env, or the main checkout's when running from a
# worktree (worktrees do not carry the gitignored .env).
ENV_FILE="$ROOT/.env"
if [ ! -f "$ENV_FILE" ]; then
  COMMON=$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)
  [ -n "$COMMON" ] && ENV_FILE="$(dirname "$COMMON")/.env"
fi

mqtt_verify() {
  set -a; . "$ENV_FILE"; set +a
  local deadline=$((SECONDS + VERIFY_TIMEOUT)) fw="" avail=""
  while [ $SECONDS -lt $deadline ]; do
    avail=$(mosquitto_sub -h "$MQTT_HOST" -p "${MQTT_PORT:-1883}" \
      -u "${MQTT_USER:-}" -P "${MQTT_PASSWORD:-}" \
      -t 'espsensor/+/availability' -v -W 4 2>/dev/null \
      | grep -v '/unknown/' | grep -m1 ' online' || true)
    if [ -n "$avail" ]; then
      local id; id=$(echo "$avail" | sed -E 's|espsensor/([^/]+)/.*|\1|')
      fw=$(mosquitto_sub -h "$MQTT_HOST" -p "${MQTT_PORT:-1883}" \
        -u "${MQTT_USER:-}" -P "${MQTT_PASSWORD:-}" \
        -t "homeassistant/sensor/${id}_temperature/config" -C 1 -W 5 2>/dev/null \
        | grep -o '"sw_version":"[^"]*"' | cut -d'"' -f4 || true)
      if [ "$fw" = "$EXPECTED_FW" ]; then
        echo "VERIFIED: $id online, running $fw ($((SECONDS))s)"
        return 0
      elif [ -n "$fw" ]; then
        # Online on a different version: either the retained discovery has not
        # refreshed yet (keep waiting) or boot-health rolled the image back.
        echo "  online but reporting $fw (want $EXPECTED_FW), waiting..."
      fi
    fi
    sleep 5
  done
  if [ -n "$fw" ]; then
    echo "DEPLOY LIKELY ROLLED BACK: device is online but reports $fw, not $EXPECTED_FW."
    echo "The new image probably never reached the broker and boot-health reverted it."
  else
    echo "DEPLOY UNVERIFIED: no online device on the broker after ${VERIFY_TIMEOUT}s."
    echo "Link recovery may still bring it back; check scripts/smoke_test.py on USB."
  fi
  return 1
}

ping_verify() {
  local deadline=$((SECONDS + VERIFY_TIMEOUT))
  while [ $SECONDS -lt $deadline ]; do
    if ping -c 1 -W 2000 -t 3 "$IP" >/dev/null 2>&1; then
      echo "VERIFIED (ping only): $IP answers ($((SECONDS))s). No broker creds to check the version."
      return 0
    fi
    sleep 5
  done
  echo "DEPLOY UNVERIFIED: $IP not answering ping after ${VERIFY_TIMEOUT}s."
  return 1
}

if [ -f "$ENV_FILE" ] && command -v mosquitto_sub >/dev/null 2>&1; then
  mqtt_verify
else
  echo "(.env or mosquitto clients unavailable - falling back to ping)"
  ping_verify
fi
