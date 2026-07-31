# Always-on mode, microSD storage and OTA updates

This covers the `feather_esp32s2_always_on` build: the device stays powered
instead of deep sleeping, samples on a timer, keeps a local history on a microSD
card, and can be updated over the network.

## The power tradeoff, stated plainly

The default build wakes, publishes, and deep sleeps. Always-on mode does not
sleep at all, which is what makes network OTA practical — a deep-sleeping node is
awake for only a few seconds per interval and an upload has essentially no chance
of landing.

Rough averages for a 3500 mAh cell, no WiFi power saving:

| Mode | Average current | Battery life | OTA reachable |
|---|---|---|---|
| Deep sleep, 5-min wakes | ~0.2 mA | months | no (only in diagnostic mode) |
| Always-on, WiFi power save | ~2 mA | ~4–8 weeks | yes |
| **Always-on, no power save (this build)** | **~40 mA** | **~3–4 days** | **yes** |

Always-on with no power saving is intended for a USB-powered node. On battery it
lasts days, not months. If you later want the middle row, enable WiFi modem sleep
and automatic light sleep — `wifi_configure_power_save()` in `wifi_manager.h` is
the hook, and it keeps the node reachable while cutting average current roughly
20x.

Sampling cadence and panel redraws are deliberately decoupled. A full e-ink
refresh visibly flashes the panel and spends one of its finite refresh cycles, so
a redraw needs both a meaningful change in the displayed values *and*
`DISPLAY_MIN_REFRESH_INTERVAL_SEC` (default 900) to have elapsed. Sampling every
5 minutes therefore does not mean redrawing every 5 minutes.

## Build flags

| Flag | Default | Meaning |
|---|---|---|
| `ALWAYS_ON` | `0` | Stay awake and sample from `loop()` instead of deep sleeping |
| `SAMPLE_INTERVAL_SEC` | `300` | Sampling cadence. Minimum 60 — the BME280 self-heats if polled harder |
| `DISPLAY_MIN_REFRESH_INTERVAL_SEC` | `900` | Floor between panel redraws |
| `FEATURE_STORAGE` | `1` | microSD history, logs, config and staged updates |
| `FEATURE_OTA` | `1` | Network OTA and apply-from-SD |
| `SD_CS_PIN` | `5` | Card chip select (D5 on the eInk FeatherWing) |
| `OTA_PORT` | `3232` | ArduinoOTA / espota listener port |
| `OTA_REQUIRE_PASSWORD` | `0` | Set to 1 to refuse network OTA unless a password is set |

`SAMPLE_INTERVAL_SEC` can also come from `sample_interval` in
`config/device.yaml`, or from `sample_interval_sec` in the card's config. Highest
precedence first:

1. `sample_interval_sec` on the SD card — applied at boot, after the compiled-in
   value, so it overrides whatever the binary was built with.
2. A `-DSAMPLE_INTERVAL_SEC=` build flag.
3. `sample_interval` in `config/device.yaml`, which the generator bakes in unless
   the build flag already defined it.
4. The 300-second default in `config.h`.

Values outside 60–3600 seconds are rejected at every level and the 300-second
default is used instead.

## microSD card

### Layout

```text
/config/device.json   runtime settings; blank fields fall back to the firmware
/data/YYYY-MM-DD.csv  one row per sample
/logs/logN.txt        rotating log, log0..log4, with index.txt naming the active one
/firmware/update.bin  a firmware image to apply at boot
```

The card is optional: when none responds, the same layout falls back to the
board's internal 960 KB `ffat` partition (formatted automatically on first use).
History, logs and staged updates work identically there; when free space drops
below 64 KB the oldest day's CSV is dropped, which caps internal history at
roughly two months. What internal flash cannot do is be pulled out and edited on
a computer — `/config/device.json` overrides remain a card feature in practice.

Format the card as a single FAT32 partition. On macOS:

```bash
diskutil eraseDisk FAT32 ESPSENSOR MBRFormat /dev/diskN
```

Confirm `diskN` with `diskutil list` first — `eraseDisk` destroys the card's
contents. The firmware creates `/data`, `/logs` and `/firmware` on first mount,
so only `/config/device.json` needs to be placed by hand.

### Config overrides

```json
{
  "room_name": "",
  "sample_interval_sec": 300,
  "wifi":    { "ssid": "", "password": "" },
  "mqtt":    { "host": "", "port": 1883, "user": "", "password": "",
               "publish_base": "", "subscribe_base": "" },
  "ota":     { "enabled": true, "password": "" },
  "storage": { "history_enabled": true, "logs_enabled": true,
               "history_retention_days": 90 }
}
```

Every field is optional. A blank string or a missing/out-of-range number is
ignored and the value compiled into the firmware is used instead, so a partly
filled card behaves exactly like no card at all. If the JSON fails to parse the
device logs the error and falls back entirely to compiled-in settings — a typo
cannot brick it. The merge rules are pinned down by
`firmware/arduino/test/test_config_merge/`.

`history_retention_days: 0` means keep everything. Pruning only removes dated
`/data/YYYY-MM-DD.csv` files, and only once NTP has given the device a
trustworthy clock.

**Credentials in this file are plaintext on removable media.** Leave the WiFi and
MQTT passwords blank if you would rather keep them compiled into the binary.

### Shared SPI bus

The card sits on the same hardware SPI bus as the e-ink panel; only the chip
selects differ (panel D9, card D5). Both drivers use SPI transactions, so they
coexist, but the card must be mounted *after* the bus exists. `storage_begin()` calls
`SPI.begin()` itself — which early-returns if the display already started the bus
— so both the display and headless builds work.

There is a third device on that bus: the Wing's 23K SRAM chip on `SRAM_CS_PIN`
(D6). Nothing here uses it, but it is the only other part that drives MISO, and
GxEPD2 — unlike Adafruit_EPD — never touches its chip select. A CS left as a
floating input is not a deasserted CS, so `storage_begin()` drives D6 high before
mounting. Symptoms of skipping this are intermittent: mounts that fail on some
power-ups and succeed on others, which reads as a failing card.

## OTA updates

### The first flash must be over USB

OTA works by the running firmware receiving the new image. A device running
firmware without OTA support cannot be updated over the network, so the build
that introduces OTA has to be installed over USB once. After that, network
updates work.

Partitioning needs no changes: the board's stock table already provides two
1408 KB app slots, and this build uses about 970 KB.

```bash
# one-time, over USB
cd firmware/arduino
../../venv/bin/python ../../scripts/gen_device_header.py
pio run -e feather_esp32s2_always_on -t upload
```

The venv path is not incidental: `gen_device_header.py` needs PyYAML and
python-dotenv to read `config/device.yaml` and `.env`, and without them it
silently falls back to defaults and produces a binary with **empty WiFi
credentials**. That device never joins the network — so it cannot be reached by
OTA either, and recovering it means another USB flash.

### Network OTA

```bash
cd firmware/arduino
pio run -e feather_esp32s2_always_on -t upload \
  --upload-port <device-ip> --project-option="upload_protocol=espota"
```

Find the device with `dns-sd -B _arduino._tcp` (macOS) or by its
`<room-name>.local` hostname.

If `ota.password` is set on the card, the password has to reach `espota`
somehow. Prefer keeping it out of your shell history and out of the argument
list where a shared machine's `ps` would show it:

```bash
read -rs OTA_PASSWORD && export OTA_PASSWORD   # prompts without echoing
pio run -e feather_esp32s2_always_on -t upload \
  --upload-port <device-ip> --project-option="upload_protocol=espota" \
  --project-option="upload_flags=--auth=\${sysenv.OTA_PASSWORD}"
```

Passing `--auth=<password>` literally also works, but it persists in shell
history, in the process list, and in CI logs. Since this password authorises
firmware replacement, do not type it inline on a shared or logged machine.

Progress is logged to serial in 10% steps. The transfer runs inside
`ArduinoOTA.handle()` for longer than the 30-second watchdog timeout, so the
watchdog is fed from the progress callback. A failed or interrupted upload leaves
the running firmware untouched — the image goes to the inactive slot and the
bootloader only swaps on a verified `Update.end()`.

**Network OTA runs unauthenticated by default** (`OTA_REQUIRE_PASSWORD=0`), a
deliberate choice for this project's private home network: OTA then works with
no setup step. Know what it costs before reusing it elsewhere — any host on the
LAN can replace this device's firmware, and an always-on node advertises itself
over mDNS. The firmware prints a prominent warning at every boot while running
this way.

To authenticate, set `ota.password` in `/config/device.json` (the same file you
are already editing for WiFi) and power cycle. Build with
`-DOTA_REQUIRE_PASSWORD=1` to turn an unset password into a hard failure instead
of a warning. Updating from the SD card never uses a password either way.

### Update from the card

Works regardless of sleep mode and needs no network.

1. Copy `.pio/build/feather_esp32s2_always_on/firmware.bin` to
   `/firmware/update.bin` on the card.
2. Insert the card and power cycle.
3. The device checks the image's magic byte, writes it to the inactive slot,
   verifies it, and reboots into it.
4. On success the file is renamed `update.applied`; on failure `update.failed`,
   and the old firmware keeps running. Either way it is not retried, so a bad
   image cannot cause a reboot loop.

## Local history

Each sample appends a row to `/data/YYYY-MM-DD.csv`:

```text
iso_time,uptime_s,temp_c,rh_pct,press_hpa,batt_v,batt_pct,rssi_dbm
```

Failed readings are written as empty cells rather than `0` or `nan`, so a
spreadsheet reads them as missing. Before NTP completes, rows are timestamped
`unsynced` and land in `/data/nodate.csv` rather than being dropped.

Both builds write history — the always-on loop once per `sample_interval_sec`,
the deep-sleep build once per wake. Retention differs only in when the sweep
runs: the always-on build folds it into its six-hourly maintenance tick, while
the deep-sleep build sweeps on the first wake of each new calendar day (the
marker lives in RTC memory, so a power cycle costs one extra directory scan).

## Device log

`storage.logs_enabled` mirrors the firmware's own `WARN` and `ERROR` output to
`/logs/logN.txt`, rotating at `SD_LOG_MAX_BYTES` across `SD_LOG_FILE_COUNT`
files with `/logs/index.txt` naming the active one. Lines are prefixed with
local time once NTP has landed, and with `+<uptime>s` before that.

`INFO` and below stay on serial only. An SD write costs milliseconds against a
`Serial.printf`'s microseconds, and boot-time INFO chatter would dominate the
card without telling you anything a reproduction on serial would not. Note that
the mirror only opens *after* the card is mounted and the config is read, so the
handful of lines emitted earlier in boot — including a mount failure itself —
are visible on serial only.
