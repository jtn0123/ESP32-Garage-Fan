#!/bin/zsh
# Garage fan weather bridge: Open-Meteo -> MQTT home/outdoor/* every 5 min.
# Replaces the defunct bridge that left a stale retained snapshot.
#
# STATUS 2026-08-10: OPTIONAL. This launchd bridge silently stopped on
# 2026-08-05 and its stale retained snapshot blinded auto mode for five days
# -- the failure that motivated firmware v1.14.19's net/weather module, which
# now polls Open-Meteo from the device itself. Running this bridge again is
# harmless (the fan takes whichever source wrote last, judged by freshness),
# but nothing depends on it anymore, and that is the point.
#
# Broker credentials and site coordinates come from ~/.local/garage-fan/env,
# which is NOT in this repo. Create it with 0600 permissions:
#
#   MQTT_HOST=...
#   MQTT_USER=...
#   MQTT_PASS=...
#   SITE_LAT=...
#   SITE_LON=...
#
# The credentials used to be hardcoded here and reached a public repo; see
# scripts/rotate_credentials.sh.
export PATH=/opt/homebrew/bin:/usr/bin:/bin

ENV_FILE="${GARAGE_FAN_ENV:-$HOME/.local/garage-fan/env}"
[[ -f "$ENV_FILE" ]] || { echo "missing $ENV_FILE" >&2; exit 1; }
set -a
. "$ENV_FILE"
set +a

: "${MQTT_HOST:?MQTT_HOST not set}"
: "${MQTT_USER:?MQTT_USER not set}"
: "${MQTT_PASS:?MQTT_PASS not set}"
: "${SITE_LAT:?SITE_LAT not set}"
: "${SITE_LON:?SITE_LON not set}"

J=$(curl -s -m 20 "https://api.open-meteo.com/v1/forecast?latitude=$SITE_LAT&longitude=$SITE_LON&current=temperature_2m,relative_humidity_2m,wind_speed_10m,weather_code&temperature_unit=fahrenheit&wind_speed_unit=ms")
[[ -z "$J" ]] && exit 1
read -r TF RH WS WC <<< "$(echo "$J" | /usr/bin/python3 -c "
import json,sys
c=json.load(sys.stdin)['current']
print(round(c['temperature_2m'],1), round(c['relative_humidity_2m']), round(c['wind_speed_10m'],1), c['weather_code'])
")"
[[ -z "$TF" ]] && exit 1
TC=$(/usr/bin/python3 -c "print(round(($TF-32)*5/9,1))")
publish() {
  local topic="$1"
  local value="$2"
  mosquitto_pub -h "$MQTT_HOST" -u "$MQTT_USER" -P "$MQTT_PASS" -r -t "home/outdoor/$topic" -m "$value"
  return $?
}
publish temp_f "$TF"
publish temp "$TC"
publish rh "$RH"
publish hum "$RH"
publish wind_mps "$WS"
publish condition_code "$WC"
publish ts "$(date +%s)"
