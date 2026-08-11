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


def history(days: int):
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

    def log_message(self, *a):
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

    def route(self):
        u = urlparse(self.path)
        p, q = u.path, parse_qs(u.query)
        STATE["uptime_s"] = int(time.time() - BOOT) + 1837

        if p == "/_scen":  # harness only
            for k, v in q.items():
                if k in SCEN:
                    SCEN[k] = (
                        None
                        if v[0] == "none"
                        else True if v[0] == "true" else False if v[0] == "false" else int(v[0])
                    )
            return self._json(200, SCEN)
        if p == "/_die":  # simulate the device going away
            SCEN["down"] = True
            return self._json(200, {"ok": True})
        if SCEN.get("down") and p.startswith("/api"):
            self.close_connection = True
            return

        if p in ("/", "/index.html"):
            return self._send(200, CONSOLE.read_bytes(), "text/html")
        if p == "/api/state":
            return self._json(200, STATE)
        if p == "/api/device":
            return self._json(200, DEVICE)
        if p == "/api/stats":
            return self._json(200, STATS)
        if p == "/api/sensors":
            return self._json(200, {"ok": True, "temp_c": 23.86, "rh": 38.9, "hpa": 999.9})
        if p == "/api/events":
            return self._send(
                200,
                "\n".join(
                    f"{int(time.time())-i*30} {1000-i*30} health rssi=-63 heap=46724 drops=0"
                    for i in range(30)
                ),
                "text/plain",
            )

        if p == "/api/history":
            d = q.get("days", [None])[0]
            if d not in ("1", "7", "30"):
                return self._json(400, {"error": "days must be 1, 7 or 30"})
            if not (SCEN["card"] and SCEN["synced"]) and d != "1":
                return self._json(503, {"error": "sd card not mounted"})
            return self._json(200, history(int(d)))

        if p == "/download.csv":
            if "days" in q:
                raw = q["days"][0]
                if not raw.isdigit() or not (1 <= int(raw) <= 30):
                    return self._json(400, {"error": "days must be 1-30"})
            return self._send(
                200,
                "epoch,temp_c,rh,hpa,outside_f,speed,batt_v,chg\n"
                "1786425931,24.00,40.0,999.9,73.2,9,4.20,1\n",
                "text/csv",
            )

        # ---- writes ------------------------------------------------------
        if p == "/api/set":
            if self._write_guard():
                return
            s = q.get("speed", [None])[0]
            if s is None or not s.lstrip("-").isdigit() or not (0 <= int(s) <= 12):
                return self._json(400, {"error": "0-12 only"})
            STATE["speed"] = int(s)
            STATE["uptime_s"] += 1  # so ordering guards see progress
            return self._json(200, STATE)
        if p == "/api/config":
            if self._write_guard():
                return
            for k, key, cast in (
                ("auto", "auto", lambda v: v != "0"),
                ("max", "auto_max", int),
                ("min", "auto_min", int),
                ("offc", "offc", float),
                ("offi", "offi", float),
            ):
                if k in q:
                    try:
                        STATE[key] = cast(q[k][0])
                    except ValueError:
                        pass
            STATE["uptime_s"] += 1
            return self._json(200, STATE)
        if p in ("/api/raw", "/api/restart", "/api/sdformat", "/api/sdpurge", "/update"):
            if self._write_guard():
                return
            if p == "/api/sdpurge":
                return self._json(403, {"error": "bad token"})
            return self._json(403, {"error": "bad token"})

        return self._json(404, {"error": "404"})


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
