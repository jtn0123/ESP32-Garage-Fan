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

import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
import os
from pathlib import Path
import time
from urllib.parse import parse_qs, urlparse

# Serve the console straight out of the build tree, so dogfooding always
# exercises the bundle that would actually ship.
CONSOLE = Path(__file__).resolve().parents[1] / "web" / "dist" / "console.html"
# Overridable so the Playwright suite and tests/test_http_contract.py can run at
# the same time without fighting over one port.
PORT = int(os.environ.get("MOCK_PORT", "8099"))
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
    # The panel before its first refresh: the firmware answers ready:false
    # until then, and the console has a branch for it that nothing could reach.
    "panel_ready": True,
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


# Both caches exist for the Playwright suite: a dozen browser contexts hammer
# this single-process server, and re-reading a 58 KB file and rebuilding 288
# rows of trig per request made the MOCK the slowest thing in the run. Tests
# then timed out waiting for the first paint, which reads as a console bug and
# is not one. Dogfooding by hand gets the same benefit for free.
#
# Both are published as a SINGLE tuple rebind, never as two field writes. This
# is a ThreadingHTTPServer: a version that stored the key first and the value
# second let another thread observe the new key beside the old (or absent)
# value, and the handler died mid-response -- the browser saw ERR_EMPTY_RESPONSE
# and the suite reported it as a console failure. Read once into a local, build
# off to the side, then swap; rebinding one name is atomic under the GIL.
_page_cache: "tuple[int, bytes] | None" = None


def console_bytes() -> bytes:
    """web/dist/console.html, re-read only when the build actually changes."""
    global _page_cache
    stamp = CONSOLE.stat().st_mtime_ns
    cached = _page_cache
    if cached is None or cached[0] != stamp:
        cached = (stamp, CONSOLE.read_bytes())
        _page_cache = cached
    return cached[1]


_hist_cache: "tuple[tuple, dict] | None" = None


def history():
    # Keyed on the knobs AND the current second: the rows carry timestamps, so a
    # cache that ignored the clock would freeze the chart's right-hand edge.
    global _hist_cache
    key = (tuple(sorted(SCEN.items())), int(time.time()))
    cached = _hist_cache
    if cached is not None and cached[0] == key:
        return cached[1]
    value = _history_uncached()
    _hist_cache = (key, value)
    return value


def _history_uncached():
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


# ------------------------------------------------------- the e-ink mirror
#
# A SYNTHETIC frame, not a reimplementation of ui/display.cpp. The console's
# mirror is a framebuffer blitter with no layout logic of its own (see
# web/src/panel.ts), so what it needs exercising is the wire shape and the
# decode: two 1-bit planes, row-major, MSB first, correct stride, red over
# black. Copying the firmware's layout here would be a second copy to keep in
# step, which is the exact trap the framebuffer design avoids.
#
# It does track STATE["speed"], so the bar and the digits move when the console
# drives the fan -- otherwise a test could not tell a live mirror from a
# hardcoded picture.
DISP_W = 250
DISP_H = 122
DISP_STRIDE = (DISP_W + 7) // 8

# 3x5 digits, one string per glyph, row-major. Enough to read a speed off the
# mock's panel; the real panel uses the Adafruit GFX font.
_FONT3X5 = {
    "0": ("111", "101", "101", "101", "111"),
    "1": ("010", "110", "010", "010", "111"),
    "2": ("111", "001", "111", "100", "111"),
    "3": ("111", "001", "111", "001", "111"),
    "4": ("101", "101", "111", "001", "001"),
    "5": ("111", "100", "111", "001", "111"),
    "6": ("111", "100", "111", "101", "111"),
    "7": ("111", "001", "001", "001", "001"),
    "8": ("111", "101", "111", "101", "111"),
    "9": ("111", "101", "111", "001", "111"),
    "-": ("000", "000", "111", "000", "000"),
    ".": ("000", "000", "000", "000", "100"),
}


class _Plane:
    """A 1-bit, row-major, MSB-first plane -- the firmware's format."""

    def __init__(self):
        self.buf = bytearray(DISP_STRIDE * DISP_H)

    def px(self, x, y):
        if 0 <= x < DISP_W and 0 <= y < DISP_H:
            self.buf[y * DISP_STRIDE + (x >> 3)] |= 0x80 >> (x & 7)

    def rect(self, x, y, w, h):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.px(xx, yy)

    def frame(self, x, y, w, h):
        for xx in range(x, x + w):
            self.px(xx, y)
            self.px(xx, y + h - 1)
        for yy in range(y, y + h):
            self.px(x, yy)
            self.px(x + w - 1, yy)

    def text(self, x, y, s, scale=1):
        for ch in s:
            glyph = _FONT3X5.get(ch)
            if glyph:
                for gy, row in enumerate(glyph):
                    for gx, bit in enumerate(row):
                        if bit == "1":
                            self.rect(x + gx * scale, y + gy * scale, scale, scale)
            x += 4 * scale


def display_frame():
    """Compose the mock's panel image: the black plane and the red plane."""
    black, red = _Plane(), _Plane()
    speed = max(0, int(STATE["speed"]))

    black.frame(0, 0, DISP_W, DISP_H)
    red.rect(0, 14, DISP_W, 2)  # the header rule, red like the firmware's
    black.rect(0, 15, DISP_W, 1)

    # Left: the two temperatures, which must DIFFER. Drawing the same value in
    # both slots would let a console that swapped or duplicated them pass.
    inside_f = STATE["outside_f"] + 8
    black.text(6, 26, f"{inside_f:.0f}", scale=4)
    black.text(6, 76, f"{STATE['outside_f']:.0f}", scale=3)

    # Right: the fan, with a red bar whose fill follows the speed.
    black.rect(126, 20, 1, 82)
    black.text(136, 30, str(speed), scale=6)
    bar_w = DISP_W - 132 - 6
    black.frame(132, 72, bar_w, 10)
    fill = int(round(speed / 12 * (bar_w - 2)))
    if fill > 0:
        red.rect(133, 73, fill, 8)
        for x in range(133, 133 + fill, 3):
            black.rect(x, 73, 1, 8)

    black.rect(0, 104, DISP_W, 1)
    return black, red


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
        # NOSONAR (pythonsecurity:S5131) -- marked at the SINK, which is where
        # the rule reports, not at the source line.
        #
        # Sonar's own flow for this names the source as CONSOLE.read_bytes() in
        # _page: the bytes of web/dist/console.html, this repo's committed
        # build artifact, served as the page. Serving a static file is the
        # whole job of that handler, so no sanitising step exists to add.
        #
        # This is the last remaining path. Query input can no longer reach a
        # response body at all: /_scen stopped echoing state, and every other
        # query-derived value lands in int(), float() or a true/false
        # comparison before it is stored.
        self.wfile.write(raw)  # NOSONAR

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
        # Sonar's S5131 flow names this read as its source; the suppression and
        # the reasoning live at the sink in _send, where the rule reports.
        return self._send(200, console_bytes(), "text/html")

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

    # Every argument handle_config() in net/web.cpp accepts. A key missing here
    # is not a harmless omission: the mock answers 200 and echoes STATE back
    # unchanged, so the console looks like it applied a setting that went
    # nowhere. onf/offf were missing exactly that way, which is why the
    # differential steppers had never been exercised end to end by anything.
    # tests/test_web_contract.py::test_mock_accepts_every_config_arg keeps this
    # in step with the firmware.
    CONFIG_KEYS = {
        "auto": ("auto", lambda v: v != "0"),
        "max": ("auto_max", int),
        "min": ("auto_min", int),
        "offc": ("offc", float),
        "offi": ("offi", float),
        "onf": ("on_f", float),
        "offf": ("off_f", float),
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

    def _display(self, _query):
        if not SCEN["panel_ready"]:
            # Exactly what handle_display() answers before the first render.
            return self._json(
                200,
                {
                    "ready": False,
                    "w": 0,
                    "h": 0,
                    "stride": 0,
                    "tricolor": False,
                    "age_s": -1,
                    "black": "",
                    "red": "",
                },
            )
        black, red = display_frame()
        return self._json(
            200,
            {
                "ready": True,
                "w": DISP_W,
                "h": DISP_H,
                "stride": DISP_STRIDE,
                # The mock stands in for the MONO part recorded in
                # docs/HARDWARE.md, so the console renders accents grey and the
                # "mono panel" note is what a test sees by default.
                "tricolor": False,
                "age_s": 42,
                "black": base64.b64encode(bytes(black.buf)).decode(),
                "red": base64.b64encode(bytes(red.buf)).decode(),
            },
        )

    def _display_refresh(self, _query):
        return self._json(200, {"ok": True})

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
        "/api/display": _display,
        "/download.csv": _csv,
        # Reads, but token-guarded: a core dump is a RAM snapshot and RAM holds
        # the token, the WiFi PSK and the MQTT password. See handle_crash().
        "/api/crash": _needs_token,
        "/api/crash.bin": _needs_token,
    }
    WRITES = {
        "/api/set": _set,
        "/api/config": _config,
        "/api/display/refresh": _display_refresh,
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
