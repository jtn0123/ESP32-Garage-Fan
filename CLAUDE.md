# ESP32-Garage-Fan

ESP32-S2 Feather replacement for the iLiving ILG8SF12V-DC shutter fan's wall
controller. The fan speaks a proprietary active-low PWM protocol over a USB-A
connector (NOT USB — never treat that link as USB, and never plug the fan's
cable into a computer: the fan drives 5 V out on VBUS).

## CRITICAL: repository boundary

This repo was derived (with full history) from `jtn0123/ESP32-Temp-Sensor`.
It is a SEPARATE project now.

- **NEVER open pull requests, issues, or comments against
  `jtn0123/ESP32-Temp-Sensor`** or any other repository. All PRs, issues,
  branches, and pushes target `jtn0123/ESP32-Garage-Fan` only.
- If a git remote for ESP32-Temp-Sensor is present locally, it exists only for
  occasional cross-repo feature comparison. Do not push to it, do not open PRs
  against it, do not comment on it.
- The two repos are intentionally similar and will diverge (different device
  UI/UX). Feature pooling between them happens as explicit, human-requested
  compare-and-port tasks — never automatically.

## What matters here

- Active firmware: `firmware/arduino`, envs `feather_esp32s2_fan_controller`
  (the product) and `feather_esp32s2_fan_rehearsal` (logic-analyzer validation).
  Both are in `default_envs`, so a bare `pio run` (and CI) builds them.
- Firmware sources are being split out of the old single file into
  `src/{config.h,fan,sensors,storage,net,ui,system}/`. Each module owns its own
  statics behind a narrow API — follow that pattern rather than adding globals
  back to `fan_controller_main.cpp`, which is mid-migration.
- The web console is TypeScript under `web/`. Never hand-edit
  `web/dist/console.html` or `src/generated_page.h`: both are build output.
- `VERSION` at the repo root is the only firmware version. Do not hardcode one
  in C++ again.
- The whole repo is the fan now. The inherited ESP32-Temp-Sensor room-node
  subsystems — e-ink display firmware, UI-spec codegen, web simulator, device
  manager, icon pipeline, CAD, ESPHome, Rust stub — were deleted in the
  fan-only cleanup. `firmware/arduino/src` holds five files. Do not resurrect
  any of it without an explicit request; port from upstream instead.
- Protocol ground truth: `docs/fan_protocol/PROTOCOL.md` + raw `.sr` captures.
  The duty table is measured, not derived — do not "clean it up". There is no
  tach or feedback line: D− was measured idling high, so anything needing RPM
  or stall detection needs new hardware first.
- OTA: POST firmware.bin to `/update?token=...` (A/B slots; ota_rollback
  confirms an image only after it reaches the MQTT broker).
- WiFi/MQTT credentials come from a gitignored `.env` at repo root via
  `scripts/gen_device_header.py` → `src/generated_config.h`. Fresh clones and
  worktrees without `.env` build with empty credentials — copy `.env` first.
- Bench hardware notes: the fan's 5 V export cannot power the Feather (browns
  out) — deployed board runs on LiPo or USB wall power. ESP32-S2 CDC drops
  serial prints unless a DTR-asserting terminal is attached; a "silent" board
  is usually fine — check mDNS (`garage-fan.local`) or `/api/state` instead.
