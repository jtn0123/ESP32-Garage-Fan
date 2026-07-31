#!/usr/bin/env bash
# Run every native (host) test env in one command. CI and `make test` call this.
# Envs are discovered from platformio.ini so a new native_* env is picked up
# automatically instead of silently skipped.
set -euo pipefail
cd "$(dirname "$0")/../firmware/arduino"

envs=$(grep -oE '^\[env:native[a-z_]*\]' platformio.ini | sed 's/\[env:\(.*\)\]/\1/')
fail=0
for e in $envs; do
  echo "=== pio test -e $e ==="
  if ! pio test -e "$e" 2>&1 | tail -2; then
    fail=1
  fi
done
exit $fail
