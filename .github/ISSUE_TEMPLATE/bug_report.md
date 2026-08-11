---
name: Bug report
about: Something in the fan controller misbehaves
title: ''
labels: ''
assignees: ''
---

## What happened

## What you expected instead

## Reproduction

1.
2.
3.

## Fan / controller state

- Firmware version (from `/api/state` or the web UI):
- Commanded speed (0–12):
- Control path used: web UI / `POST /api/set` / MQTT `garage/fan/set`
- Board power source: LiPo / USB wall / other

## Evidence

<!-- `curl http://garage-fan.local/api/state`, MQTT payloads, serial log.
     Note that ESP32-S2 CDC drops serial prints unless a DTR-asserting terminal
     is attached -- a silent board is usually fine, check mDNS or /api/state. -->

```
```

## Environment

- Host OS:
- PlatformIO / `pio` version:
