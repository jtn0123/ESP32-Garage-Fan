## ESP32 eInk Room Node — Battery‑Friendly Wi‑Fi Sensor with 2.13" e‑ink

"ESP32 eInk Room Node" — battery‑efficient Wi‑Fi sensor node with a 2.13" mono e‑ink display. Wakes on a schedule, reads inside sensors, pulls outside stats (from Home Assistant or MQTT), renders to e‑ink (partial), publishes to HA, and deep‑sleeps.

### Hardware

- **MCU**: Adafruit **Feather ESP32‑S2** (USB‑C, Li‑ion charge). Alternatives: XIAO ESP32‑C3, LILYGO T5.
- **Display**: Adafruit **2.13" mono e‑ink FeatherWing**, **250×122** px (partial refresh supported).
- **Sensors**: SHT40 or BME280 (I2C). Optional SGP40 (VOC), SCD41 (CO₂ single‑shot).
- **Battery**: 1S Li‑ion/LiPo (1000/3500/7000 mAh). JST‑PH 2.0.
- **Cables**: If your pack uses micro JST 1.25, use a 1.25F→PH2.0M adapter.

### Display & Layout

- Resolution: **250×122**, mono 1‑bit.
- Regions & partial windows: see `firmware/arduino/src/display_layout.h`.
- Refresh: **partial** each wake; **full** every N cycles to clear ghosting.
 - Panel driver (GxEPD2): start with `GxEPD2_213_B74` (SSD1680 122×250). If your Wing revision differs, try `GxEPD2_213_DEPG0213BN` and other 122×250 classes listed in `GxEPD2_display_selection.h`.
- Layout parity: the geometry JSON is versioned and hashed. The generator emits `LAYOUT_VERSION`, `LAYOUT_CRC`, and `LAYOUT_MD5` in `display_layout.h`. Firmware prints these in the USB metrics line and publishes `sensors/<room>/layout` (retained) so you can verify the web sim and device are on the same layout at a glance.

### Shared Display Geometry (single source of truth)

- `config/display_geometry.json` defines the canvas size and all drawing rectangles.
- Consumers:
  - Web simulator loads it at runtime (falls back to `web/sim/geometry.json`).
  - Python PNG mock reads it to render a matching 250×122 preview.
  - Firmware header is generated from it.
- Firmware auto-generation:
  - PlatformIO now runs `scripts/gen_layout_header.py` automatically as a pre-build step for all Arduino envs. You do not need to call it manually.
  - The generated header is `firmware/arduino/src/display_layout.h`.
- Manual run (optional): only if you want to re-generate outside of PlatformIO:

```bash
python3 scripts/gen_layout_header.py
```

- Icon bitmaps:
  - Weather/moon icons are generated from SVGs by `scripts/convert_icons.py` into `firmware/arduino/src/icons_generated.h`.
  - This is not run every build (icons don't change often). Re-run only after changing the SVGs in `web/icons/mdi/`:

```bash
python3 scripts/convert_icons.py
```

- Tests validate that the mock and sim are in sync (snapshot + pixel checks).

Pixel/snapshot checks (CI, blocking):

- Python mock PNG golden and Playwright screenshot are compared with a small tolerance (≤5 pixels) to absorb minor font raster differences across environments.
- Goldens are bootstrapped on first run; subsequent diffs beyond tolerance will fail CI.

### Power & Duty Cycle (example)

- Deep sleep: ~**0.09 mA** (board LEDs/rails off).
- Awake (Wi‑Fi + draw): **60–100 mA** for **30–120 s**.
- Hourly / 2‑hourly / 4‑hourly wakes supported; see `/scripts/power_estimator.py`.

There is also an **always‑on** mode (`env:feather_esp32s2_always_on`) that never
sleeps, samples every 5 minutes, and stays reachable for network OTA. It averages
roughly **40 mA** — about **3–4 days** on a 3500 mAh cell — so it suits a
USB‑powered node. See [docs/ALWAYS_ON_AND_OTA.md](docs/ALWAYS_ON_AND_OTA.md).

### Home Assistant Integration

- **ESPHome path**: uses `homeassistant:` to read outdoor entities and native sensors to publish.
- **MQTT path**: subscribe to retained `home/outdoor/...` topics; publish inside readings to `sensors/<room>/...`.
  - Supports optional MQTT auth: set `mqtt.user` and `mqtt.password` in `config/device.yaml`.
  - Example HA automation to publish outdoor values is provided in `homeassistant/mqtt_outdoor_publish.yaml`.
  
  MQTT Discovery (Arduino build) — what the device publishes/uses:
  - Discovery topics (retained):
    - `homeassistant/sensor/<client_id>_inside_temp/config`
    - `homeassistant/sensor/<client_id>_inside_hum/config`
  - Discovery payload keys (abridged): `state_topic`, `availability_topic`, `payload_available: "online"`, `payload_not_available: "offline"`, `unit_of_measurement` ("°F" for temp), `device_class`, `state_class: "measurement"`, `suggested_display_precision` (1 for °F, 0 for %), and `expire_after` (slightly above the wake interval) so HA greys stale values if a wake is missed.
  - Device info included for HA device registry: `identifiers`, `name`, `manufacturer` (DIY), `model` (Feather ESP32-S2), and `sw_version`.
  - State topics (retained):
    - `sensors/<room>/inside/temp` (Fahrenheit, one decimal)
    - `sensors/<room>/inside/hum` (percent, integer)
  - Availability (retained):
    - `sensors/<room>/availability` with payloads `online`/`offline` (LWT set on connect, offline published just before sleep).

### Configuration

Create `config/device.yaml` from the sample:

```yaml
room_name: "Office"
wake_interval: "2h"           # 1h|2h|4h
full_refresh_every: 12         # partials between full clears
outside_source: "ha"          # ha|mqtt
ha_entities:
  outdoor_temp: sensor.outdoor_temperature
  outdoor_hum:  sensor.outdoor_humidity
  weather:      weather.home
mqtt:
  host: 192.168.1.10
  base_topics:
    publish: sensors/office
    subscribe: home/outdoor
wifi:
  ssid: YOUR_SSID
  password: YOUR_PASS
thresholds:
  temp_degC: 0.1
  rh_pct: 1
battery:
  capacity_mAh: 3500
  sleep_current_mA: 0.09
  active_current_mA: 80

Notes:
- `sw_version` is published from the generated `FW_VERSION` define. You can override by setting `fw_version` in `config/device.yaml` or exporting `FW_VERSION` in the environment before building. Otherwise it falls back to `git describe`/short SHA or `dev`.
```

### Security & Credential Management

This project implements production-grade security practices to protect credentials:

#### Credential Storage

**Never commit credentials to version control!** This project uses environment variables to keep secrets safe:

1. **Create `.env` file** (git-ignored):
   ```bash
   cp .env.example .env
   # Edit .env with your actual credentials:
   # WIFI_SSID, WIFI_PASSWORD, MQTT_USER, MQTT_PASSWORD
   ```

2. **Configuration files** (`config/device.yaml`) contain only non-sensitive settings. Credentials are loaded from environment variables by the build process.

3. **Generated headers** (`firmware/arduino/src/generated_config.h`) are git-ignored to prevent accidental commits.

#### Security Features

- **Pre-commit Hook**: Automatically scans staged files for credentials before allowing commits
  ```bash
  # Enable the security hook (one-time setup)
  ./scripts/setup_git_hooks.sh
  ```

- **Staged-Only Scanning**: The hook only checks files you're committing, not your entire repo
- **Smart Exclusions**: Test files and examples are intelligently excluded from security scans
- **CI Security Scanning**: GitHub Actions runs Gitleaks on all PRs to catch any missed credentials

#### Credential Rotation

If credentials are ever exposed:

```bash
# Use the rotation helper script
./scripts/rotate_credentials.sh

# This will guide you through:
# 1. Changing passwords on your services
# 2. Updating .env with new credentials
# 3. Cleaning up any exposed files
```

#### Safe Logging

The firmware includes credential sanitization to prevent accidental password logging:

```cpp
#include "credential_safe.h"

// Logs "Bat***" instead of full SSID
LOG_INFO("WiFi: %s", sanitize_ssid(WIFI_SSID).c_str());

// Logs "**********" instead of password
LOG_INFO("Pass: %s", sanitize_credential(WIFI_PASS).c_str());
```

#### Security Workflow

1. **Development**: Keep credentials in `.env` (local only)
2. **Commits**: Pre-commit hook blocks credential leaks
3. **CI/CD**: GitHub Actions scans for secrets
4. **Production**: Use secure credential storage on device

#### Bypassing Security Checks

In rare cases where you need to commit something flagged as a false positive:

```bash
# NOT RECOMMENDED - Use only when certain it's safe
git commit --no-verify
```

### Build — Arduino / PlatformIO

See `firmware/arduino/platformio.ini`:

```ini
[env:feather_esp32s2]
platform = espressif32
board = featheresp32-s2
framework = arduino
monitor_speed = 115200
lib_deps =
  ZinggJM/GxEPD2 @ ^1.5.0
  adafruit/Adafruit GFX Library @ ^1.11.9
  adafruit/Adafruit BusIO @ ^1.16.1
  bblanchon/ArduinoJson @ ^7
  knolleary/PubSubClient @ ^2.8
  adafruit/Adafruit BME280 Library @ ^2.2.4
  adafruit/Adafruit Unified Sensor @ ^1.1.14
  adafruit/Adafruit MAX1704X @ ^1.0.3   ; optional, set USE_MAX17048 1
build_flags =
  -DCORE_DEBUG_LEVEL=1
  -DEINK_WIDTH=250 -DEINK_HEIGHT=122
```

Note: The optional status LED heartbeat uses the Adafruit NeoPixel library and is already included in the PlatformIO `lib_deps`. You can disable the LED at build time with `-DUSE_STATUS_PIXEL=0` if optimizing for lowest sleep current.

Developer builds:
- `env:feather_esp32s2_always_on`: stays awake, samples every `SAMPLE_INTERVAL_SEC` (default 300), keeps a CSV history and rotating logs on microSD, and accepts network OTA. See [docs/ALWAYS_ON_AND_OTA.md](docs/ALWAYS_ON_AND_OTA.md) for the power tradeoff and the OTA workflow.
- `env:feather_esp32s2_headless`: headless with parity to e‑ink build (same thresholds and availability; sleeps per `WAKE_INTERVAL_SEC`).
- `env:feather_esp32s2_dev2`: headless, 3 min awake / 3 min sleep cycle for soak testing while limiting heat.
- `env:feather_esp32s2_headless_1h`: headless, 1‑hour sleep schedule (WAKE_INTERVAL_SEC=3600).
- `env:feather_esp32s2_headless_always`: headless, always on (DEV_NO_SLEEP=1) for rapid MQTT/HA validation.

### Flash helper script (recommended)

Use `scripts/flash.py` to build and upload common modes without remembering env names. You can choose sleep cadence and headless/display with flags.

```bash
# 1-hour sleep (default)
python3 scripts/flash.py

# 2-hour sleep
python3 scripts/flash.py --mode 2h

# 3-minute sleep
python3 scripts/flash.py --mode 3m

# Always-on (no sleep)
python3 scripts/flash.py --mode always

# Headless variants (no e-ink)
python3 scripts/flash.py --headless --mode 1h
python3 scripts/flash.py --headless --mode always

# Optional: specify a serial port explicitly
python3 scripts/flash.py --mode 3m --port /dev/cu.usbmodem101

# Build only, do not upload
python3 scripts/flash.py --mode always --build-only
```

Notes:
- Modes are implemented via environment overrides to the generated `generated_config.h`:
  - `--mode 3m|1h|2h` sets `WAKE_INTERVAL` for config generation.
  - `--mode always` sets `-DDEV_NO_SLEEP=1`.
- `--headless` toggles the PlatformIO environment to `feather_esp32s2_headless`; otherwise `feather_esp32s2_display_only` is used for fast layout iteration.
- Auto‑detect upload port is used unless `--port` is provided.

Commands:
```bash
# 3-on / 3-off dev cycle build
pio run -e feather_esp32s2_dev2
PORT=$(ls /dev/cu.usbmodem* | head -n1)
pio run -t upload -e feather_esp32s2_dev2 --upload-port "$PORT"
pio device monitor -p "$PORT" -b 115200
```

Minimal `src/main.cpp` skeleton:

```cpp
#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("ESP32 eInk Room Node boot");
}

void loop() {
  // placeholder loop
  delay(1000);
}
```

### Display Simulators

- Python PNG mock: renders a 250×122 preview to `out/display_mock.png`.

```bash
# optional: use the repo venv if created
source .venv/bin/activate 2>/dev/null || true
python scripts/mock_display.py
open out/display_mock.png  # macOS
```

- Web Canvas simulator: Interactive page with advanced debugging tools.

```bash
# Automatic port selection (recommended)
python3 scripts/start_simulator.py
# or
./scripts/start_simulator.sh

# Manual start on specific port
cd web/sim
python3 -m http.server 8080
# open http://localhost:8080/index.html in your browser
```

Both simulators read the same `config/display_geometry.json`, so they match the firmware layout.

#### Web Simulator Features

The web simulator now includes comprehensive debugging and testing tools:

**UI Validation Panel** - Real-time detection of:
- Text overflow issues (e.g., "1013 hPa" exceeding bounds)
- Region collisions and overlaps
- Alignment problems
- Font sizing issues

**Advanced Debug Tools** - Interactive testing features:
- **Performance Monitor**: FPS, render times, frame counting
- **Test Scenarios**: 40+ pre-configured edge cases including:
  - Text overflow tests
  - Battery states (0-100%, charging)
  - All weather icons
  - Missing data scenarios
  - Extreme sensor values
- **Data Editor**: Live editing of all display fields
- **Visual Testing**: Capture baselines, compare changes, show pixel diffs
- **Icon Tester**: Quick buttons for all weather conditions
- **State Inspector**: View internal data and metrics

#### Weather Icons (24×24 PNG)

- Generated into `config/icons/` via:

```bash
source .venv/bin/activate 2>/dev/null || true
python scripts/gen_icons.py
```

#### Simulator Tests

Run Python tests (image checks + snapshot):

```bash
source .venv/bin/activate 2>/dev/null || true
python -m pip install -U pytest pillow
pytest -q
```

Optional: Web simulator pixel test (requires Playwright):

```bash
python -m pip install -U playwright pytest-playwright
python -m playwright install --with-deps chromium
pytest -q -k web_sim
```

#### UI Validation and Testing

Run comprehensive UI validation tests:

```bash
# Real-time UI validation with detailed report
python3 scripts/ui_validation_engine.py --save

# Visual regression testing for icons and UI elements
pytest tests/test_visual_regression.py -v

# Test debug panel integration
pytest tests/test_debug_panel_integration.py -v

# Run all UI validation tests
pytest tests/test_ui_validation.py -v

# Analyze layout for issues (overlaps, gaps, alignment)
python3 scripts/visual_layout_analyzer.py
```

The validation engine detects:
- Text overflow beyond region bounds
- Unintended region collisions
- Alignment inconsistencies
- Font size issues
- Content clipping at edges

Visual regression tests verify:
- All weather icons render correctly
- Battery indicator states (0-100%)
- Text handling for edge cases
- Missing data display
- UI element alignment

### ESPHome (optional path)

See `firmware/esphome/room_node.yaml` for a starting YAML.

ESPHome Quickstart:

1. Install ESPHome and add a new device using board `featheresp32-s2`.
2. Copy `firmware/esphome/room_node.yaml` into your ESPHome configuration.
3. If the e‑ink renders incorrectly, change `display.model:` between `2.13in`, `2.13in-ttgo`, or `2.13in_v2`.
4. Provide `secrets.yaml` with `wifi_ssid` and `wifi_password`.
5. Optionally set `ota_password` in `secrets.yaml`.
6. Flash and verify the layout matches the Arduino build (250×122).

See `firmware/esphome/secrets.example.yaml` for a sample secrets file.

### Rust (optional path)

See `firmware/rust/` for a sketch `Cargo.toml` and `src/main.rs`.

### Pin Map

See `hardware/pinmap.md` for Feather ESP32‑S2 + 2.13" FeatherWing.

### License

MIT (see `LICENSE`).

### Headless environments

- `env:feather_esp32s2_headless`: headless build that mirrors the e‑ink build's thresholds and availability sequencing. It uses the same wake/sleep cadence as the e‑ink build (via `WAKE_INTERVAL_SEC` from config) and publishes `availability` online on connect and `offline` just before sleep.
- `env:feather_esp32s2_dev2`: headless 3/3 developer soak cycle (stays awake ~3m, then sleeps ~3m) for MQTT/HA soak testing.
- `env:feather_esp32s2_headless_always`: headless, always-on (no sleep) for rapid iteration.

Commands:

```bash
# 1-hour sleep (default)
python3 scripts/flash.py

# 3-minute cycle (awake ~3m, sleep ~3m)
python3 scripts/flash.py --mode 3m

# Always-on (no sleep)
python3 scripts/flash.py --mode always

# Optional: specify a serial port explicitly
python3 scripts/flash.py --mode 3m --port /dev/cu.usbmodem101

# Build only, do not upload
python3 scripts/flash.py --mode always --build-only
```

#### Status LED heartbeat (optional)

- When awake, the firmware can animate the onboard NeoPixel as a visual heartbeat for headless debugging.
- Enabled by default via `-DUSE_STATUS_PIXEL=1`. It is ticked while the device is awake and turned off before deep sleep.
- To disable entirely, build with `-DUSE_STATUS_PIXEL=0` (recommended for lowest sleep current).

LED pin and board notes:

- The firmware uses `PIN_NEOPIXEL` automatically if your board definition provides it (e.g., Adafruit Feather ESP32‑S2). No action needed in that case.
- If your NeoPixel is wired to a different GPIO, override with a build flag:

  ```ini
  build_flags =
    -DUSE_STATUS_PIXEL=1
    -DSTATUS_PIXEL_PIN=5    ; replace 5 with your NeoPixel data pin
  ```

- Brightness defaults to a low value for debugging. To change it, edit `firmware/arduino/src/main.cpp` and adjust the value passed to `s_statusPixel.setBrightness(...)`.
- Power: the LED is turned off before deep sleep. For the absolute lowest sleep current, disable via `-DUSE_STATUS_PIXEL=0`.
- If your board has a discrete 3‑pin RGB LED (not a NeoPixel), this heartbeat won’t drive it. In that case either:
  - disable the NeoPixel heartbeat and add a simple PWM heartbeat on your R/G/B pins, or
  - let me know your LED pins and I’ll switch the firmware to a PWM‑based status LED for your board.

### Switching wake interval (1h / 2h / 4h)

- Long‑term duty cycle is configured in `config/device.yaml`:

```yaml
wake_interval: "2h"   # examples: "1h", "2h", "4h", or a number of seconds like "3600"
```

- After editing, rebuild and upload the normal environment:

```bash
pio run -e feather_esp32s2
pio run -t upload -e feather_esp32s2 --upload-port PORT
```

- The build step generates `firmware/arduino/src/generated_config.h` with `WAKE_INTERVAL_SEC` based on your YAML.

### Flashing via software (force bootloader) and upload

You can force the Feather ESP32‑S2 into bootloader mode from the CLI and upload without touching buttons.

Steps (macOS):

```bash
# 1) Find the current modem port
pio device list   # look for /dev/cu.usbmodemXXXX

# 2) Force the UF2 bootloader by touching the CDC port at 1200 baud
stty -f /dev/cu.usbmodemXXXX 1200
sleep 2  # give it time to re-enumerate

# 3) Upload to the new modem port (it often changes to /dev/cu.usbmodem01)
NEW_PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -n1)
pio run -t upload -e feather_esp32s2_headless --upload-port "$NEW_PORT"
```

All‑in‑one snippet:

```bash
port=/dev/cu.usbmodemXXXX
stty -f "$port" 1200 || true
sleep 2
p=$(ls /dev/cu.usbmodem* 2>/dev/null | head -n1)
pio run -t upload -e feather_esp32s2_headless --upload-port "$p"
```

Alternative (serial ROM bootloader via esptool):

```bash
python -m esptool --chip esp32s2 --port /dev/cu.usbmodemXXXX chip_id
# then upload with PlatformIO on that same port
pio run -t upload -e feather_esp32s2_headless --upload-port /dev/cu.usbmodemXXXX
```

### Monitor over USB and MQTT

- USB (115200 baud):

```bash
pio device monitor -p /dev/cu.usbmodemXXXX -b 115200
```

- USB metrics helper script (parses one-line JSON emitted by firmware):

```bash
source .venv/bin/activate 2>/dev/null || true
pip install -r requirements.txt
python3 scripts/monitor_usb.py /dev/cu.usbmodemXXXX --raw
```

Example parsed output (monitor script prints Fahrenheit by default):

```
ip=192.168.1.23  tempF=72.28  rh%=41  wifi=up  mqtt=up  battV=4.08  batt%=87
```

#### Serial command interface (optional)

- The firmware accepts simple commands over USB when running the headless/always‑on build.
- Open a monitor, type a command, press Enter:

```bash
pio device monitor -p /dev/cu.usbmodemXXXX -b 115200
```

Supported commands:

```text
help               # list commands
status             # ip, wifi/mqtt, battery estimate
metrics            # emit one JSON metrics line
wifi               # force Wi‑Fi reconnect
mqtt               # force MQTT reconnect
sleep <sec>        # deep sleep for N seconds immediately
reboot             # software reset
pub <tempF> <rh%>  # publish inside readings (Fahrenheit + RH)
```

Notes:

- Only one serial program can access the port at a time. Close other monitors before opening a new one.

- MQTT (replace with your broker/user/pass and base topic):

```bash
mosquitto_sub -h <MQTT_HOST> -u <USER> -P '<PASS>' -t 'sensors/<room>/#' -v
```

- Helper script in this repo:

```bash
python3 scripts/mqtt_headless_check.py --host <MQTT_HOST> --user <USER> --password <PASS> \
  --pub-base sensors/<room> --sub-base home/outdoor --seed
```

### Troubleshooting

- If the upload tool picks the wrong port, pass `--upload-port /dev/cu.usbmodemXXXX` explicitly.
- If the port flaps or disappears, unplug/replug USB, try another cable/port, or re‑enter bootloader and retry.
- Hardware fallback: hold BOOT, tap RESET, release BOOT; then upload to the new modem port.

### Secrets safety

- Store Wi‑Fi/MQTT credentials only in `config/device.yaml`. That file and the generated `firmware/arduino/src/generated_config.h` are already ignored by Git.

### Dev-cycle sleep/wake checklist

- Manual sleep test over USB:
  - Send `sleep <sec>` (e.g., `sleep 180`).
  - Expect USB: `Sleeping for 180s` followed by a retained MQTT `status` payload like `sleeping=1 sec=180`.
  - On wake, USB prints include `Reset: ..., Wake: ...` and `RTC wake count: N` (increments each cycle).
  - MQTT `sensors/<room>/debug` JSON includes `rtc_wake_count` and `wakeup_cause` (expect `TIMER`).

- Rails timing:
  - Display and sensor rails are powered off before deep sleep with a short 5 ms discharge delay.
  - On boot, if the e‑ink rail is gated, it is enabled and given a 10 ms settle before driver init.

- Normal cycle:
  - Before each scheduled sleep, firmware publishes a retained `status` heartbeat: `sleeping=1 sec=<WAKE_INTERVAL_SEC>`.
  - Broker availability topic also flips to `offline` just before sleep; Home Assistant will grey stale entities slightly after `expire_after`.

- If a wake is missed or partial:
  - Check USB logs for last printed lines and the `Timeouts mask`.
  - Verify MQTT retained `status` and the last `debug` payload; confirm `rtc_wake_count` changes across cycles.
  - If issues persist, try disabling partial refresh (USB: `mode full`) to isolate e‑ink timing, or set `-DUSE_STATUS_PIXEL=0` to minimize rail load.

### Credits

- Phosphor Icons (Fill), MIT — weather symbols vendored for this project. See `web/icons/mdi/` and `web/icons/third_party/` if present. Source: https://phosphoricons.com/?q=weather&size=52
- Icon pipeline inspired by esp32-weather-epd: https://github.com/lmarzen/esp32-weather-epd/tree/main/icons


