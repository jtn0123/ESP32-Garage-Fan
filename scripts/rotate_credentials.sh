#!/usr/bin/env bash
# Credential rotation checklist.
#
# This script deliberately contains NO real values. The version it replaces
# printed the actual WiFi SSID, the broker host and username, and the first
# characters of both the WiFi and MQTT passwords -- in a public repository. A
# helper written to respond to an exposure must not itself be one, and a
# password prefix is a material head start for an attacker.
#
# It reports only WHICH keys are present in .env, never their values.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENV_FILE="$ROOT/.env"

echo "Credential rotation checklist"
echo "============================="
echo

if [ -f "$ENV_FILE" ]; then
  echo "Keys currently defined in .env (values not shown):"
  # Print key names only. Never echo the right-hand side.
  grep -oE '^[A-Za-z_][A-Za-z0-9_]*=' "$ENV_FILE" 2>/dev/null \
    | tr -d '=' | sed 's/^/  - /' | sort
else
  echo "No .env at repo root. Copy .env.example and fill it in:"
  echo "  cp .env.example .env"
fi

cat <<'EOF'

Rotate at the source first
--------------------------
  1. Change the password on the MQTT broker itself.
  2. Change the WiFi password if it may have been exposed.
     Rotating only the copies in this repo changes nothing: the credential is
     whatever the broker and the access point will still accept.

Then update every consumer
--------------------------
  3. This repo's .env
  4. Any sibling checkout's .env (worktrees do not inherit it)
  5. ~/.local/garage-fan/env  (the launchd helpers in docs/fan_protocol/mac/)
  6. Home Assistant, and any other MQTT client on the network
  7. Rebuild and redeploy the firmware so the device carries the new value:
       make build && make deploy IP=<device>
     The build regenerates src/generated_config.h from .env; that file is
     gitignored and must never be committed or attached to a release.

If a credential ever reached a commit
-------------------------------------
  8. Rotation is the fix. History rewriting is secondary and incomplete:
     GitHub keeps a permanent refs/pull/<n>/head for every pull request ever
     opened, and those refs survive a force-push and a branch deletion. They
     can only be removed by GitHub Support or by deleting the repository.
     Assume any value that was pushed publicly is permanently disclosed and
     rotate it; treat a rewrite as tidying, not remediation.

  9. Verify the value is gone from the working tree and from history:
       git grep -n '<value>' HEAD
       git log --all --oneline -S'<value>'
EOF
