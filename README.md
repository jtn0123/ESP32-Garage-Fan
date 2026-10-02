# ESP32-Garage-Fan

An ESP32-S2 Feather that replaces the wall controller of an iLiving
ILG8SF12V-DC 12" shutter fan, after reverse-engineering the proprietary
control link iLiving runs over a USB-A connector.

**The link is not USB.** It's 5 V power plus active-low PWM at ~100.7 Hz on
the D+ pin — fan power equals the low fraction of each 9934 µs period, with a
measured 13-entry duty table (`docs/fan_protocol/PROTOCOL.md`, raw logic-
analyzer captures in `docs/fan_protocol/captures/`). Never plug the fan's
cable into a computer: the fan drives 5 V out on VBUS.

## Features

- **Fan Console** at `http://garage-fan.local/` — differential-first: garage
  and yard temperature with the gap between them, a gauge placing that gap
  against the engage/release band, speed 0–12, and a 24 h / 7 d / 30 d chart
  you can drag across to read any moment. The PWM readout opens a live scope
  of the gate-drive waveform next to the captured duty table, and Settings
  edits the auto thresholds, probe offsets and maintenance actions in place.
- **Update check** — the console asks GitHub Releases whether a newer tag
  exists and links the `.bin`; the controller itself never talks to the
  internet, so there is no TLS stack on the device
- **HTTP API** — reads are `GET`: `/api/state`, `/api/device` (duty table,
  identity, broker), `/api/history?days=1|7|30`, `/api/stats`. Writes are
  **`POST` only** — `/api/set?speed=0..12`, `/api/config`, `/api/raw`,
  `/api/restart`, `/api/sdformat`, `/api/sdpurge` — and they additionally
  refuse a request carrying a foreign `Origin`, since a cross-origin HTML form
  can `POST` without a preflight. Requests with no `Origin` at all (curl, the
  deploy script) are allowed, so add `-X POST` and nothing else
- **MQTT** — `garage/fan/set` / `garage/fan/state` / `garage/fan/availability`,
  retained commands resume after power loss
- **OTA updates** — `POST /update with X-Fan-Token`, written to the inactive A/B
  slot; an image is confirmed only after it reaches the broker, and an
  unconfirmed image rolls back automatically after three failed boots
- Network-loss-safe: the RMT peripheral keeps transmitting the last speed on
  its own hardware, even during flash writes

## Uptime Kuma

The current live monitor is [Garage fan controller #30](http://10.27.27.99:3001/dashboard/30)
under Devices & network. It uses **HTTP(s) - Json Query** against
`http://10.27.27.187/api/state`, so it works with the existing 1.26.0 firmware.
The JSONata expression is below, with operator `==` and expected value `true`:

```text
$boolean(mqtt = true and confirmed = true and (auto = false or $type(outside_f) = "number") and (plug = null or (plug.age_s >= 0 and plug.age_s <= 120 and plug.verdict != -1 and plug.cycling = false)))
```

It checks every 60 seconds with a 20-second timeout and two retries. The live
state API does not expose indoor freshness; the new endpoint below adds that
check after a firmware deployment. At setup there were no Kuma notification
channels configured, so this monitor records status but sends no alerts.

The read-only `GET /health` endpoint is intended for an HTTP monitor. It
returns HTTP **200** with `status: "up"` when MQTT is connected and the running
firmware image is confirmed. In automatic mode, both the indoor and outdoor
temperature readings must also be fresh. Otherwise it returns HTTP **503**
with `status: "down"`; the `mqtt`, `confirmed`, `auto`, `inside_fresh`,
`outside_fresh`, `power_ok` and `actuator_ok` flags identify the failing dependency. An enabled
power meter must have a reading no more than 120 seconds old and report neither
a power disagreement nor cycling. Manual mode does not require temperature
data. The check reads cached state, performs no sensor or
network requests, and needs no password. Missing SD storage or an optional
barometer does not make the controller unavailable.

On [your Uptime Kuma instance](http://10.27.27.99:3001/), switch the existing
monitor to these settings after deploying firmware that includes the endpoint:

| Setting | Value |
|---|---|
| Monitor type | HTTP(s) |
| Friendly name | Garage fan controller |
| URL | `http://10.27.27.187/health` (your current device) |
| Method | GET |
| Heartbeat interval | 60 seconds |
| Request timeout | 20 seconds |
| Retries | 2 |
| Accepted status codes | `200` only |

Use a reserved device IP reachable **from the Kuma server**; `.local` names
often do not resolve inside containers. Select an existing notification
channel if alerts are wanted. Boot, OTA, and the first outdoor-weather fetch
can initially report down until the dependencies are ready. This check proves
controller readiness, not blade rotation: there is no tachometer feedback.

Kuma API keys authenticate the read-only `/metrics` endpoint, using HTTP Basic
authentication with an empty username and the key as password. They cannot
create monitors or sign in to the dashboard; monitor setup requires an
authenticated dashboard session. Never put a Kuma key into the firmware or
browser bundle. See [Kuma API keys](https://github.com/louislam/uptime-kuma/wiki/Prometheus-API-Keys).

## Hardware

Feather ESP32-S2 → BSS138 level shifter → USB-A screw-terminal breakout →
fan cable. GPIO 18 (A0) carries the PWM through the shifter (the signal is
~4.3–5 V — not 3.3 V-safe); the fan's 5 V feeds the shifter's HV reference.
Power the Feather from LiPo or USB wall power — the fan's 5 V export can't
carry it alone — a battery buffers it (and charges from it).
**Complete wiring guide, pin map, diagrams, and rebuild checklist:**
[docs/HARDWARE.md](docs/HARDWARE.md); protocol internals and captures in
`docs/fan_protocol/`.

## Build

Use Python 3.11+, PlatformIO from `requirements-firmware.txt`, and Bun 1.3.14
for console work. Install the Python tools and the pinned browser first:

```sh
python -m pip install -r requirements.txt -r requirements-firmware.txt -r requirements-dev.txt
cd web
bun install --frozen-lockfile --ignore-scripts
bun run e2e:browsers
cd ..
```

The production-handler pytest regressions also require a C++17 compiler
(Xcode command-line tools on macOS, or `g++` on Linux). Rust/Cargo is needed
only for `cargo test --manifest-path tools/fantape/Cargo.toml`.

```
make build          # build the fan controller firmware
make flash          # build + flash over USB
make deploy IP=...  # build + OTA + verify (defaults to garage-fan.local)
make test           # native Unity tests + pytest
make web            # typecheck + test + rebuild the console bundle (needs Bun)
```

The version in `VERSION` is the single source of truth: `gen_device_header.py`
turns it into `FW_VERSION`, the firmware reports it on `/api/state`, and the
release workflow refuses to publish a tag that disagrees with it.

**Bumping `VERSION` on `main` publishes a release.** `tag-release.yml` creates
the matching `vX.Y.Z` tag, which triggers `release.yml` to build and attach the
images. This is automatic because it previously was not: the repo tagged
v1.14.0 and then went 22 versions without another tag, so every device's update
check spent that time comparing itself against a feed that had stopped moving.
Bump `VERSION` in the commit you actually want released, not before.

The web console lives in `web/` as TypeScript and is bundled into one
self-contained HTML file at `web/dist/console.html`, which **is committed** —
the firmware build runs with only Python and PlatformIO, so `pio run` never
needs Bun. A PlatformIO pre-script gzips that file into
`src/generated_page.h` (52 KB of HTML becomes 18 KB of flash) and the device
serves it with `Content-Encoding: gzip`. If you edit anything under `web/src`,
run `make web` and commit the regenerated bundle; CI fails the PR otherwise.

WiFi/MQTT credentials come from a gitignored `.env` at the repo root (copy
`.env.example`), which `scripts/gen_device_header.py` turns into
`src/generated_config.h` during the build. A clone without `.env` builds with
empty credentials — `make deploy` refuses to ship such an image.

Privileged requests use the `X-Fan-Token` header. Provisioning passwords and
new tokens belong in POST form bodies. Never put credentials in a URL.
An empty token, the former public default, or the example placeholder disables
administrator access. Set a private 6–38 character `FAN_OTA_TOKEN` in `.env`
for a local image, or retain an already valid private NVS token. The console
asks for the token when its field is blank; it has no built-in token.
Older firmware accepts query authentication only: use its existing updater or
USB for the first upgrade, then use the new console/deploy tooling. For an old
device still using the public default, a one-time legacy OTA can carry a
private-token image; that image persists its private token into NVS at boot.
Only the former public constant may appear in that migration URL. Private
tokens must never be put in URLs. Multipart form authentication cannot bridge
the old upload callback, which runs before form arguments are merged.

Credentials now migrate from legacy NVS keys into one versioned snapshot.
Validation or a failed NVS write leaves the previous snapshot intact and
returns an error without rebooting. Rolling back to pre-snapshot firmware
restores its legacy credentials; plan network changes accordingly.
PWM failures return HTTP 503, keep the last accepted command, and report
`actuator_fault` in state plus `actuator_ok: false` in health. This detects
rejected LEDC commands, not blade motion.

Console test dependencies override nanoid to 3.3.19 and undici to 8.11.2 to
clear the audited advisories while upstream packages update their ranges.

## Lineage

Derived from [ESP32-Temp-Sensor](https://github.com/jtn0123/ESP32-Temp-Sensor)
with full history. The inherited e-ink room-node subsystems (display firmware,
UI-spec codegen, web simulator, device manager, weather/moon icon pipeline, CAD
enclosure) were removed in the fan-only cleanup — this repo now contains just
the fan controller. Recover any of it from history or from the upstream repo.
See `CLAUDE.md` for the repository-boundary rules.
