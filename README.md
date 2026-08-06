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

- **Web UI** at `http://garage-fan.local/` — speed 0–12, live status
- **HTTP API** — `GET /api/state`, `GET /api/set?speed=0..12`
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
carry it. Full wiring and bring-up history in `docs/fan_protocol/`.

## Lineage

Derived from [ESP32-Temp-Sensor](https://github.com/jtn0123/ESP32-Temp-Sensor)
with full history; the projects share bones and will diverge. See `CLAUDE.md`
for the repository-boundary rules.
