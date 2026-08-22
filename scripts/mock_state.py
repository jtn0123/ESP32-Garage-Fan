"""Shared mutable state for the mock device: the spec'd wire payloads and
the scenario knobs. Split from mock_device.py at the 500-line ceiling; every
sibling module mutates these same dicts, which is the point -- the mock IS
one process-wide state machine."""

from __future__ import annotations

import time
from typing import Any, Tuple, Union

# The wire payloads are heterogeneous by design (ints, floats, bools, nested
# dicts, None), so per-key typing here would be dishonest; the real contract
# lives in web/src/types.ts, pinned by tests/test_web_contract.py.
Json = dict[str, Any]

STEP = 300
BOOT = time.time()

STATE: Json = {
    "speed": 9,
    "auto": True,
    "auto_max": 9,
    "auto_min": 0,
    "on_f": 2.5,
    "off_f": 1.5,
    "outside_f": 73.2,
    "fw": "1.14.23",
    "slot": "ota_0",
    "confirmed": True,
    "unhealthy_boots": 0,
    "sensor": True,
    "last_reset": "sw_reset",
    "boots": 41,
    "prev_death": "sw_reset",
    "sd_q": False,
    "sd_total_mb": 28887,
    "sd_used_mb": 28677,
    "sd_free_mb": 210,
    "batt": {"v": 4.195, "pct": 100, "chg": True, "eta_h": None, "mvh": -6},
    "rssi": -63,
    "drops": 0,
    "mqtt": True,
    "uptime_s": 1837,
    "ip": "127.0.0.1",
    # The Tapo watt meter on the fan's supply, as net/plug reports it. The
    # SCEN plug knob swaps in the disagreement case; "none" serves null, the
    # no-meter build.
    "plug": {
        "w": 20.3, "v": 120.9, "age_s": 3, "verdict": 1, "cycling": False, "flips": 0,
        "expect_w": 20.3, "implied_spd": 9,
    },  # fmt: skip
    # Gas boost, as fan/control reports it: enabled with defaults, latch idle.
    "gas_on": True,
    "gas_spd": 6,
    "gas_voc": 250,
    "gas_active": False,
    "wh_today": 412.5,
    "cost_kwh": 0.15,
}
DEVICE: Json = {
    "id": "garage-fan-d69dbe",
    "host": "garage-fan",
    "repo": "jtn0123/ESP32-Garage-Fan",
    # Placeholders on purpose. This file is public and a mock has no reason to
    # carry the real broker address or the site's actual SSID.
    "broker": "192.0.2.10:1883",
    "ssid": "example-wifi",
    "mqtt_user": "fan",
    "lat": "",
    "lon": "",
    "topic_set": "garage/fan/set",
    "topic_out": "home/outdoor/temp_f",
    "period_us": 9934,
    "sample_s": STEP,
    "high_us": [0, 3477, 4072, 4868, 5066, 5661, 6159, 6754, 7251, 7847, 8344, 8940, 9437],
}
STATS: Json = {
    "run_today_s": 16501,
    "run_total_s": 410634,
    "energy_wh": 5183,
    "wh_today": 412.5,
    "watts_now": 47,
    "t_min_f": 75.1,
    "t_max_f": 77.1,
    "t_avg_f": 75.9,
    "samples": 288,
}

# Harness knobs.
SCEN: Json = {
    "card": True,
    "synced": True,
    # Rows the CARD HOLDS, not rows a response returns: /api/history reads the
    # requested window out of this and decimates it to 288 points, the way
    # sdcard::read_range does. 17280 is 60 days at the 5-minute cadence, i.e. a
    # card that can fill every range the console offers -- the default has to
    # be a full card, or the long ranges are untestable here for the same
    # reason they were broken there.
    "rows": 60 * 288,
    "gap_at": None,
    "corrupt": False,
    "flat_rh": False,
    "down": False,
    # The panel before its first refresh: the firmware answers ready:false
    # until then, and the console has a branch for it that nothing could reach.
    "panel_ready": True,
    # What a token-accepted POST /update "flashes": the fw the mock reports
    # afterwards (boots+1, other slot, confirmed). None = the upload is taken
    # and the board "reboots" onto the SAME version, which is what a rolled-back
    # image looks like from the console. Drives the one-click install specs.
    "ota_fw": None,
    # The fw the board reports. A knob so a worker's mock can be put back after
    # an /update "flashed" it; applying it writes STATE["fw"] directly.
    "fw": "1.14.23",
    # "ok" agree, "bad" sustained disagreement, "cycling" the fan switching
    # itself on and off at a held speed (the 2026-08-20 night), "none" no
    # meter at all
    "plug": "ok",
}


# The measured watt baseline per speed (net/plug.cpp's kBaselineW). The mock
# answers /api/state and /api/plugtrace from it so "expected vs measured" is
# a real comparison here rather than two unrelated numbers.
PLUG_BASELINE = [1.4, 2.5, 3.9, 4.9, 7.0, 7.6, 10.3, 12.8, 15.4, 20.3, 23.6, 30.8, 37.8]


# Type and bounds for every scenario knob. /_scen coerces through this rather
# than storing whatever arrived, so nothing a caller sends survives as a string
# anywhere in this process.
# A knob is either a bare bool, an ("choice", *values) enum, or an
# (int, lo, hi) range.
# A runtime alias, so spelled with Union: the e2e harness runs this mock on
# the system /usr/bin/python3 (3.9 on macOS), where `type | tuple[...]` is a
# TypeError at import time even though mypy reads it fine.
ScenSpec = Union[type, Tuple[str, ...], Tuple[type, int, int]]

SCEN_SPEC: dict[str, ScenSpec] = {
    "card": bool,
    "synced": bool,
    "rows": (int, 0, 60 * 288),  # up to a full 60-day card
    "gap_at": (int, 0, 8640),  # or None; an index into the RETURNED rows
    "corrupt": bool,
    "flat_rh": bool,
    "down": bool,
    "panel_ready": bool,
    "plug": ("choice", "ok", "bad", "cycling", "none"),
    "ota_fw": ("version",),  # X.Y.Z or none
    "fw": ("version",),  # what the board reports; resets after an /update
}


def coerce_scen(key: str, raw: str) -> bool | int | str | None:
    """Whitelisted parse of one knob. Raises ValueError on anything else."""
    spec = SCEN_SPEC[key]
    if isinstance(spec, tuple) and spec[0] == "version":
        if raw == "none":
            return None
        # Split-and-isdigit rather than a regex: the value is caller-supplied,
        # and `\d+\.\d+\.\d+` is the classic polynomial-backtracking shape.
        parts = raw.split(".")
        if len(raw) > 32 or len(parts) != 3 or not all(p.isdigit() for p in parts):
            raise ValueError(f"{key} takes X.Y.Z or none")
        return raw
    if isinstance(spec, tuple) and spec[0] == "choice":
        choices = tuple(str(c) for c in spec[1:])
        if raw not in choices:
            raise ValueError(f"{key} takes one of {'|'.join(choices)}")
        return raw
    if spec is bool:
        if raw not in ("true", "false"):
            raise ValueError(f"{key} takes true|false")
        return raw == "true"
    if not isinstance(spec, tuple):
        raise ValueError(f"{key} has a malformed spec")  # unreachable with the table above
    lo, hi = spec[1], spec[2]
    if not isinstance(lo, int) or not isinstance(hi, int):
        raise ValueError(f"{key} has a malformed spec")  # unreachable with the table above
    if raw == "none":
        return None
    if not raw.isdigit() or not (lo <= int(raw) <= hi):
        raise ValueError(f"{key} takes {lo}..{hi} or none")
    return int(raw)
