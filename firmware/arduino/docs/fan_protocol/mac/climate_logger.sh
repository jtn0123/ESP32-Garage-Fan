#!/bin/zsh
# Garage fan forever-logger: append every garage/climate publish to CSV.
export PATH=/opt/homebrew/bin:/usr/bin:/bin
LOG=~/.local/garage-fan/climate_log.csv
[ -f "$LOG" ] || echo "epoch,payload" >> "$LOG"
mosquitto_sub -h ***REDACTED-BROKER-HOST*** -u mqtt-client -P ***REMOVED-MQTT-PASSWORD*** -t garage/climate | while read -r line; do
  echo "$(date +%s),$line" >> "$LOG"
done
