#!/usr/bin/env bash
# Build the always-on firmware and deploy it over the air.
#   scripts/deploy.sh [device-ip]
# Locates espota.py inside the installed arduino-esp32 platform package, which
# is otherwise a path nobody should have to remember. (The old advice of
# `pio run -t upload --project-option=...` no longer parses on pio 6.x.)
set -euo pipefail
IP="${1:-10.27.27.187}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

pio run -d "$ROOT/firmware/arduino" -e feather_esp32s2_always_on

ESPOTA=$(find ~/.platformio/packages -name espota.py -path "*arduinoespressif32*" | head -1)
[ -n "$ESPOTA" ] || { echo "espota.py not found - is the espressif32 platform installed?"; exit 1; }

python3 "$ESPOTA" -i "$IP" -p 3232 \
  -f "$ROOT/firmware/arduino/.pio/build/feather_esp32s2_always_on/firmware.bin" -r
echo
echo "Deployed to $IP - device reboots now; expect it back online in ~30s."
