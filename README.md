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
- **HTTP API** — `GET /api/state`, `GET /api/set?speed=0..12`,
  `GET /api/device` (duty table, identity, broker), `GET /api/history?days=`,
  `GET /api/stats`
- **MQTT** — `garage/fan/set` / `garage/fan/state` / `garage/fan/availability`,
  retained commands resume after power loss
- **OTA updates** — `POST /update?token=...`, written to the inactive A/B
  slot; an image is confirmed only after it reaches the broker, and an
  unconfirmed image rolls back automatically after three failed boots
- Network-loss-safe: the RMT peripheral keeps transmitting the last speed on
  its own hardware, even during flash writes

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

```
make build          # build the fan controller firmware
make flash          # build + flash over USB
make deploy IP=...  # build + OTA + verify (defaults to garage-fan.local)
make test           # native Unity tests + pytest
make web            # typecheck + test + rebuild the console bundle (needs node)
```

The version in `VERSION` is the single source of truth: `gen_device_header.py`
turns it into `FW_VERSION`, the firmware reports it on `/api/state`, and the
release workflow refuses to publish a tag that disagrees with it.

The web console lives in `web/` as TypeScript and is bundled into one
self-contained HTML file at `web/dist/console.html`, which **is committed** —
the firmware build runs with only Python and PlatformIO, so `pio run` never
needs node. A PlatformIO pre-script gzips that file into
`src/generated_page.h` (52 KB of HTML becomes 18 KB of flash) and the device
serves it with `Content-Encoding: gzip`. If you edit anything under `web/src`,
run `make web` and commit the regenerated bundle; CI fails the PR otherwise.

WiFi/MQTT credentials come from a gitignored `.env` at the repo root (copy
`.env.example`), which `scripts/gen_device_header.py` turns into
`src/generated_config.h` during the build. A clone without `.env` builds with
empty credentials — `make deploy` refuses to ship such an image.

## Lineage

Derived from [ESP32-Temp-Sensor](https://github.com/jtn0123/ESP32-Temp-Sensor)
with full history. The inherited e-ink room-node subsystems (display firmware,
UI-spec codegen, web simulator, device manager, weather/moon icon pipeline, CAD
enclosure) were removed in the fan-only cleanup — this repo now contains just
the fan controller. Recover any of it from history or from the upstream repo.
See `CLAUDE.md` for the repository-boundary rules.
