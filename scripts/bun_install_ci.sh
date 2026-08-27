#!/usr/bin/env bash
# Install the console's dependencies in CI, and explain the one failure that
# is not a real failure.
#
# --frozen-lockfile is the `npm ci` guarantee: fail rather than silently
# resolve something bun.lock did not pin. --ignore-scripts because these jobs
# install dependencies onto a runner that then executes them.
#
# The wrinkle is Dependabot. It updates web/package.json but cannot update
# bun.lock (dependabot-core#11602), so every grouped JS bump arrives with the
# two out of step and the install refuses it:
#
#     error: lockfile had changes, but lockfile is frozen
#
# That is the check working. But read cold, on a PR nobody wrote, it looks
# like the dependency bump broke the build rather than like a lockfile that
# needs one command. So say which it is.
set -euo pipefail

if bun install --frozen-lockfile --ignore-scripts; then
  exit 0
fi

cat <<'MSG'
::error::web/bun.lock does not match web/package.json.

If this is a Dependabot PR, nothing is broken: Dependabot updates package.json
but cannot update bun.lock yet (dependabot-core#11602). Check the branch out
and run:

    bun --cwd web install
    bun --cwd web run build     # esbuild may reminify dist/console.html
    git commit -am 'chore: refresh bun.lock'

Otherwise: someone edited package.json without running `bun install`.
MSG
exit 1
