#!/bin/zsh
# Garage fan forever-logger: append every garage/climate publish to CSV.
#
# Broker credentials come from ~/.local/garage-fan/env, which is NOT in this
# repo. Create it with 0600 permissions:
#
#   MQTT_HOST=...
#   MQTT_USER=...
#   MQTT_PASS=...
#
# They used to be hardcoded here and reached a public repo; see
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

LOG=~/.local/garage-fan/climate_log.csv
[[ -f "$LOG" ]] || echo "epoch,payload" >> "$LOG"
mosquitto_sub -h "$MQTT_HOST" -u "$MQTT_USER" -P "$MQTT_PASS" -t garage/climate | while read -r line; do
  echo "$(date +%s),$line" >> "$LOG"
done
