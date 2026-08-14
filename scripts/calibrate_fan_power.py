#!/usr/bin/env python3
"""Walk the fan through every speed and record what the watt meter says.

The fan link is fire-and-forget (no tach, no ack -- see PROTOCOL.md), so a
Tapo P110M on the fan's supply is the only ground truth that the motor did
what the controller commanded. This script builds the baseline table that
makes that comparison possible:

    HA_URL=http://<ha>:8123 HA_TOKEN=... python3 scripts/calibrate_fan_power.py

Reads watts from Home Assistant (the plug is Matter-commissioned there; its
native protocol is TPAP, which nothing open speaks yet). Steps 0, 1..12, 0 --
OFF is measured twice, first for the idle baseline and last to prove the walk
ended where it started. At each step it polls until the reading settles
(three consecutive samples within a band), then records the median.

Writes docs/fan_power_baseline.json and prints a markdown table. Restores
speed 0 + auto mode even on Ctrl-C: the one thing a calibration run must not
do is wander off leaving the fan pinned at 12.

Env:
    HA_URL / HA_TOKEN   required -- where Home Assistant lives, and auth
    HA_POWER_ENTITY     optional -- else the first sensor whose id mentions
                        both "power" and a P110/tapo-ish name is offered
    FAN_HOST            optional -- default 10.27.27.187
    SETTLE_S            optional -- max seconds to wait per step (default 90)
"""

import json
import os
import statistics
import sys
import time
import urllib.request

HA_URL = os.environ.get("HA_URL", "").rstrip("/")
HA_TOKEN = os.environ.get("HA_TOKEN", "")
ENTITY = os.environ.get("HA_POWER_ENTITY", "")
FAN = os.environ.get("FAN_HOST", "10.27.27.187")
SETTLE_S = int(os.environ.get("SETTLE_S", "90"))
BAND_W = 1.5  # three consecutive samples within this many watts = settled
OUT = os.path.join(os.path.dirname(__file__), "..", "docs", "fan_power_baseline.json")


def _http(url, method="GET", token=None, timeout=10):
    req = urllib.request.Request(url, method=method)
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(req, timeout=timeout) as r:  # NOSONAR - LAN
        return json.load(r)


def ha_states():
    return _http(f"{HA_URL}/api/states", token=HA_TOKEN)


def find_entity():
    if ENTITY:
        return ENTITY
    hits = [
        s["entity_id"]
        for s in ha_states()
        if s["entity_id"].startswith("sensor.")
        and "power" in s["entity_id"]
        and s.get("attributes", {}).get("unit_of_measurement") in ("W", "watt", "watts")
    ]
    if len(hits) == 1:
        return hits[0]
    print("Set HA_POWER_ENTITY to one of:" if hits else "No watt sensors found in HA.")
    for h in hits:
        print("   ", h)
    sys.exit(2)


def watts(entity):
    s = _http(f"{HA_URL}/api/states/{entity}", token=HA_TOKEN)
    try:
        return float(s["state"])
    except (KeyError, ValueError):
        return None


def set_speed(v):
    _http(f"http://{FAN}/api/set?speed={v}", method="POST")


def set_auto(on):
    _http(f"http://{FAN}/api/config?auto={1 if on else 0}", method="POST")


def settle(entity, label):
    """Poll until three consecutive readings sit inside BAND_W, else time out."""
    t0 = time.time()
    window = []
    readings = []
    while time.time() - t0 < SETTLE_S:
        w = watts(entity)
        if w is not None:
            readings.append(w)
            window = (window + [w])[-3:]
            print(f"  {label}: {w:7.1f} W", flush=True)
            if len(window) == 3 and max(window) - min(window) <= BAND_W:
                return statistics.median(window), readings
        time.sleep(3)
    if not readings:
        sys.exit(f"{label}: no readings from {entity} in {SETTLE_S}s")
    print(f"  {label}: never settled inside {BAND_W} W; recording the median anyway")
    return statistics.median(readings[-5:]), readings


def main():
    if not HA_URL or not HA_TOKEN:
        sys.exit("HA_URL and HA_TOKEN are required (put them in .env, then "
                 "`set -a; source .env; set +a` before running)")
    entity = find_entity()
    print(f"watt sensor: {entity}")
    print(f"fan:         {FAN}")
    print("This run disables auto and drives the fan through 0,1..12,0.")

    table = {}
    try:
        set_auto(False)
        for step, speed in enumerate([0] + list(range(1, 13)) + [0]):
            key = f"{speed}" if step == 0 or speed != 0 else "0_return"
            set_speed(speed)
            time.sleep(4)  # give the motor a head start before polling
            median, _ = settle(entity, f"speed {speed}")
            table[key] = round(median, 1)
            print(f"speed {speed}: {median:.1f} W")
    finally:
        # Whatever happened, do not leave the fan pinned.
        try:
            set_speed(0)
            set_auto(True)
            print("restored: speed 0, auto on")
        except OSError as e:
            print(f"RESTORE FAILED ({e}) -- set the fan yourself: "
                  f"curl -X POST 'http://{FAN}/api/set?speed=0'")

    out = {
        "entity": entity,
        "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "settle_band_w": BAND_W,
        "watts_by_speed": table,
    }
    with open(OUT, "w") as f:
        json.dump(out, f, indent=2)
        f.write("\n")
    print(f"\nwrote {os.path.normpath(OUT)}\n")
    print("| speed | watts |")
    print("|------:|------:|")
    for k, v in table.items():
        print(f"| {k} | {v} |")


if __name__ == "__main__":
    main()
