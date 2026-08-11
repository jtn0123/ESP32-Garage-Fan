#!/usr/bin/env python3
"""A stand-in for the fan's HTTP API, for dogfooding the console offline.

    python3 scripts/mock_device.py      # then open http://127.0.0.1:8099

Serves web/dist/console.html plus the /api surface, so the console can be
driven through states the real device rarely reaches -- an unmounted card, a
99%-full card, an outage in the middle of the history, a dead controller --
without touching the fan in the garage.

It implements the SPEC, not the firmware. If the console misbehaves against
this, either the console is wrong or the spec is; a divergence between this and
the real device is exactly what an on-hardware pass has to catch, so passing
here is necessary and not sufficient.

Scenario knobs are flipped at runtime:

    curl "http://127.0.0.1:8099/_scen?card=false"     # card unmounted -> 503
    curl "http://127.0.0.1:8099/_scen?gap_at=100"     # an outage mid-history
    curl "http://127.0.0.1:8099/_scen?flat_rh=true"   # dead-flat humidity
    curl "http://127.0.0.1:8099/_scen?rows=3"         # nearly-empty card
    curl "http://127.0.0.1:8099/_die"                 # controller goes away
"""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
from pathlib import Path
import time
from urllib.parse import parse_qs, urlparse

# Serve the console straight out of the build tree, so dogfooding always
# exercises the bundle that would actually ship.
CONSOLE = Path(__file__).resolve().parents[1] / "web" / "dist" / "console.html"
PORT = 8099
# NOSONAR: plain HTTP is the point. This mocks an ESP32 that has no TLS stack
# (see net/weather.h for the measured reason), it binds to loopback only, and
# the console's own Origin check is one of the behaviours under test -- these
# strings ARE the allowed origins. Same decision already recorded in
# scripts/deploy.sh for the same rule.
SELF_ORIGINS = {f"http://127.0.0.1:{PORT}", f"http://localhost:{PORT}"}

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
}


def coerce_scen(key: str, raw: str):
    """Whitelisted parse of one knob. Raises ValueError on anything else."""
    spec = SCEN_SPEC[key]
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


def history():
    n = SCEN["rows"]
    end = int(time.time())
    ts, temp, rh, hpa, out, batt, spd, chg = [], [], [], [], [], [], [], []
    t = end - (n - 1) * STEP
    for i in range(n):
        if SCEN["gap_at"] is not None and i == SCEN["gap_at"]:
            t += STEP * 40  # a dark stretch
        ts.append(t)
        t += STEP
        temp.append(round(24 + math.sin(i / 20) * 2.0, 1))
        rh.append(40 if SCEN["flat_rh"] else 38 + (i % 5))
        hpa.append(round(999.9 + math.cos(i / 30) * 0.4, 1))
        out.append(None if i % 17 == 0 else round(73 + math.sin(i / 25) * 4, 1))
        batt.append(round(4.20 - (i % 40) * 0.005, 2))
        spd.append(i % 10)
        chg.append(1 if i % 3 else 0)
    if SCEN["corrupt"]:  # what a bad CSV row must become
        temp[5] = None
        hpa[7] = None
    return {
        "source": "sd" if SCEN["card"] else "ring",
        "interval_s": STEP,
        "ts": ts,
        "temp_c": temp,
        "rh": rh,
        "hpa": hpa,
        "out_f": out,
        "batt_v": batt,
        "spd": spd,
        "chg": chg,
    }


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        # Silence BaseHTTPRequestHandler's per-request stderr line. The console
        # polls every 15 s and redraws on every frame, so the default access
        # log buries the tracebacks this harness exists to surface.
        pass

    def _send(self, code, body, ctype="application/json"):
        raw = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def _json(self, code, obj):
        self._send(code, json.dumps(obj))

    # --- the guards the firmware now enforces -----------------------------
    def _origin_ok(self):
        o = self.headers.get("Origin")
        return o is None or o in SELF_ORIGINS

    def _write_guard(self):
        """POST-only + Origin, matching web.cpp. Returns True if refused."""
        if self.command != "POST":
            self._json(404, {"error": "404"})
            return True
        if not self._origin_ok():
            self._json(403, {"error": "cross-origin request refused"})
            return True
        return False

    def do_GET(self):
        self.route()

    def do_POST(self):
        self.route()

    # --- routing ----------------------------------------------------------
    # Split into small handlers rather than one long if-chain: the chain grew
    # to a cognitive complexity of 67, which is exactly the shape that hides a
    # missing guard.

    def route(self):
        STATE["uptime_s"] = int(time.time() - BOOT) + 1837
        u = urlparse(self.path)
        path, query = u.path, parse_qs(u.query)

        if path.startswith("/_"):
            return self._harness(path, query)
        if SCEN["down"] and path.startswith("/api"):
            self.close_connection = True  # the controller has gone away
            return
        handler = self.READS.get(path) or self.WRITES.get(path)
        if handler is None:
            return self._json(404, {"error": "404"})
        if path in self.WRITES and self._write_guard():
            return
        return handler(self, query)

    # --- harness ----------------------------------------------------------
    def _harness(self, path, query):
        if path == "/_die":
            SCEN["down"] = True
            return self._json(200, {"ok": True})
        if path != "/_scen":
            return self._json(404, {"error": "404"})
        # Strictly whitelisted: only known keys, only known value shapes.
        applied = 0
        for key, values in query.items():
            if key not in SCEN_SPEC:
                return self._json(400, {"error": "unknown knob"})
            try:
                SCEN[key] = coerce_scen(key, values[0])
            except ValueError as exc:
                return self._json(400, {"error": str(exc)})
            applied += 1
        # Deliberately does NOT echo the knob state back.
        #
        # It used to, which made this endpoint reflect caller-controlled data
        # into its own response. The whitelist above meant no caller string
        # could actually survive the round trip -- every path lands in int(),
        # float() or a true/false comparison -- but the echo bought nothing:
        # the harness sets knobs and then observes the CONSOLE, not this
        # response. Removing it deletes the whole question instead of arguing
        # about whether the sanitising was thorough enough.
        return self._json(200, {"ok": True, "applied": applied})

    # --- reads ------------------------------------------------------------
    def _page(self, _query):
        return self._send(200, CONSOLE.read_bytes(), "text/html")

    def _state(self, _query):
        return self._json(200, STATE)

    def _device(self, _query):
        return self._json(200, DEVICE)

    def _stats(self, _query):
        return self._json(200, STATS)

    def _sensors(self, _query):
        return self._json(200, {"ok": True, "temp_c": 23.86, "rh": 38.9, "hpa": 999.9})

    def _events(self, _query):
        now = int(time.time())
        body = "\n".join(
            f"{now - i * 30} {1000 - i * 30} health rssi=-63 heap=46724 drops=0" for i in range(30)
        )
        return self._send(200, body, "text/plain")

    def _history(self, query):
        days = query.get("days", [None])[0]
        if days not in ("1", "7", "30"):
            return self._json(400, {"error": "days must be 1, 7 or 30"})
        if days != "1" and not (SCEN["card"] and SCEN["synced"]):
            return self._json(503, {"error": "sd card not mounted"})
        return self._json(200, history())

    def _csv(self, query):
        if "days" in query:
            raw = query["days"][0]
            if not raw.isdigit() or not (1 <= int(raw) <= 30):
                return self._json(400, {"error": "days must be 1-30"})
        body = (
            "epoch,temp_c,rh,hpa,outside_f,speed,batt_v,chg\n"
            "1786425931,24.00,40.0,999.9,73.2,9,4.20,1\n"
        )
        return self._send(200, body, "text/csv")

    # --- writes -----------------------------------------------------------
    def _set(self, query):
        raw = query.get("speed", [None])[0]
        if raw is None or not raw.isdigit() or not (0 <= int(raw) <= 12):
            return self._json(400, {"error": "0-12 only"})
        STATE["speed"] = int(raw)
        STATE["uptime_s"] += 1  # so the console's ordering guard sees progress
        return self._json(200, STATE)

    CONFIG_KEYS = {
        "auto": ("auto", lambda v: v != "0"),
        "max": ("auto_max", int),
        "min": ("auto_min", int),
        "offc": ("offc", float),
        "offi": ("offi", float),
    }

    def _config(self, query):
        for arg, (key, cast) in self.CONFIG_KEYS.items():
            if arg not in query:
                continue
            try:
                STATE[key] = cast(query[arg][0])
            except ValueError:
                return self._json(400, {"error": f"bad {arg}"})
        STATE["uptime_s"] += 1
        return self._json(200, STATE)

    def _needs_token(self, _query):
        # Token-guarded on the real device; the mock always refuses, which is
        # what the console's error path should be exercised against.
        return self._json(403, {"error": "bad token"})

    READS = {
        "/": _page,
        "/index.html": _page,
        "/api/state": _state,
        "/api/device": _device,
        "/api/stats": _stats,
        "/api/sensors": _sensors,
        "/api/events": _events,
        "/api/history": _history,
        "/download.csv": _csv,
    }
    WRITES = {
        "/api/set": _set,
        "/api/config": _config,
        "/api/raw": _needs_token,
        "/api/restart": _needs_token,
        "/api/sdformat": _needs_token,
        "/api/sdpurge": _needs_token,
        "/update": _needs_token,
    }


if __name__ == "__main__":
    # Loopback only, and plain HTTP by design: the device this stands in for
    # serves plain HTTP and cannot do otherwise. NOSONAR (S5332), matching the
    # decision already documented in scripts/deploy.sh.
    ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()  # NOSONAR
