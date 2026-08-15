"""Shared mutable state for the mock device: the spec'd wire payloads and
the scenario knobs. Split from mock_device.py at the 500-line ceiling; every
sibling module mutates these same dicts, which is the point -- the mock IS
one process-wide state machine."""

import time

STEP = 300
BOOT = time.time()

STATE = {
    "speed": 9,
    "auto": True,
    "auto_max": 9,
    "auto_min": 0,
    "on_f": 2.5,
    "off_f": 1.5,
    "outside_f": 73.2,
    "toff": -8.0,
    "offc": -8.0,
    "offi": -8.0,
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
    "plug": {"w": 20.3, "v": 120.9, "age_s": 3, "verdict": 1},
    "sht_pref": True,
    # Gas boost, as fan/control reports it: enabled with defaults, latch idle.
    "gas_on": True,
    "gas_spd": 6,
    "gas_voc": 250,
    "gas_active": False,
    "wh_today": 412.5,
    "cost_kwh": 0.15,
}
DEVICE = {
    "id": "garage-fan-d69dbe",
    "host": "garage-fan",
    "repo": "jtn0123/ESP32-Garage-Fan",
    # Placeholders on purpose. This file is public and a mock has no reason to
    # carry the real broker address or the site's actual SSID.
    "broker": "192.0.2.10:1883",
    "ssid": "example-wifi",
    "topic_set": "garage/fan/set",
    "topic_out": "home/outdoor/temp_f",
    "period_us": 9934,
    "sample_s": STEP,
    "high_us": [0, 3477, 4072, 4868, 5066, 5661, 6159, 6754, 7251, 7847, 8344, 8940, 9437],
}
STATS = {
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
SCEN = {
    "card": True,
    "synced": True,
    "rows": 288,
    "gap_at": None,
    "corrupt": False,
    "flat_rh": False,
    "down": False,
    # The panel before its first refresh: the firmware answers ready:false
    # until then, and the console has a branch for it that nothing could reach.
    "panel_ready": True,
    # "ok" agree, "bad" sustained disagreement, "none" no meter at all
    "plug": "ok",
}


# Type and bounds for every scenario knob. /_scen coerces through this rather
# than storing whatever arrived, so nothing a caller sends survives as a string
# anywhere in this process.
SCEN_SPEC = {
    "card": bool,
    "synced": bool,
    "rows": (int, 0, 8640),
    "gap_at": (int, 0, 8640),  # or None
    "corrupt": bool,
    "flat_rh": bool,
    "down": bool,
    "panel_ready": bool,
    "plug": ("choice", "ok", "bad", "none"),
}


def coerce_scen(key: str, raw: str):
    """Whitelisted parse of one knob. Raises ValueError on anything else."""
    spec = SCEN_SPEC[key]
    if isinstance(spec, tuple) and spec[0] == "choice":
        if raw not in spec[1:]:
            raise ValueError(f"{key} takes one of {'|'.join(spec[1:])}")
        return raw
    if spec is bool:
        if raw not in ("true", "false"):
            raise ValueError(f"{key} takes true|false")
        return raw == "true"
    _, lo, hi = spec
    if raw == "none":
        return None
    if not raw.isdigit() or not (lo <= int(raw) <= hi):
        raise ValueError(f"{key} takes {lo}..{hi} or none")
    return int(raw)
