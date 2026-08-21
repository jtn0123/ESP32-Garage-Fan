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

from __future__ import annotations

import base64
from collections.abc import Callable
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
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

# The SELF_ORIGINS block above is config, not code, hence the late import.
import sys as _sys  # noqa: E402

# Path bootstrap: this file is run as a script (pytest spawns it by path,
# Playwright's harness too) AND loaded via importlib by tests/test_qr_v1.py,
# so the sibling modules are found relative to THIS file, not the cwd.
_sys.path.insert(0, str(Path(__file__).resolve().parent))

from mock_data import boots, console_bytes, history  # noqa: E402
from mock_panel import (  # noqa: E402
    DISP_H,
    DISP_STRIDE,
    DISP_W,
    display_frame,
    qr_v1_encode,  # noqa: F401  (re-exported for tests/test_qr_v1.py)
)
from mock_state import BOOT, DEVICE, SCEN, SCEN_SPEC, STATE, STATS, coerce_scen  # noqa: E402

# What parse_qs hands every handler: each query key to its list of values.
Query = dict[str, list[str]]

CONFIG_H = Path(__file__).resolve().parents[1] / "firmware" / "arduino" / "src" / "config.h"


def firmware_default_token() -> str:
    """FAN_OTA_TOKEN's compiled default, straight from config.h."""
    m = re.search(r'#define FAN_OTA_TOKEN "([^"]*)"', CONFIG_H.read_text())
    if not m or not m.group(1):
        raise RuntimeError(f"FAN_OTA_TOKEN default not found in {CONFIG_H}")
    return m.group(1)


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args: object) -> None:
        # Silence BaseHTTPRequestHandler's per-request stderr line. The console
        # polls every 15 s and redraws on every frame, so the default access
        # log buries the tracebacks this harness exists to surface.
        pass

    def _send(self, code: int, body: bytes | str, ctype: str = "application/json") -> None:
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

    def _json(self, code: int, obj: object) -> None:
        self._send(code, json.dumps(obj))

    # --- the guards the firmware now enforces -----------------------------
    def _origin_ok(self) -> bool:
        o = self.headers.get("Origin")
        return o is None or o in SELF_ORIGINS

    def _write_guard(self) -> bool:
        """POST-only + Origin, matching web.cpp. Returns True if refused."""
        if self.command != "POST":
            self._json(404, {"error": "404"})
            return True
        if not self._origin_ok():
            self._json(403, {"error": "cross-origin request refused"})
            return True
        return False

    def do_GET(self) -> None:
        self.route()

    def do_POST(self) -> None:
        self.route()

    # --- routing ----------------------------------------------------------
    # Split into small handlers rather than one long if-chain: the chain grew
    # to a cognitive complexity of 67, which is exactly the shape that hides a
    # missing guard.

    def route(self) -> None:
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
    def _harness(self, path: str, query: Query) -> None:
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
            if key == "fw" and SCEN["fw"]:
                STATE["fw"] = SCEN["fw"]  # the one knob that is state, not scenery
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
    def _page(self, _query: Query) -> None:
        # Sonar's S5131 flow names this read as its source; the suppression and
        # the reasoning live at the sink in _send, where the rule reports.
        return self._send(200, console_bytes(), "text/html")

    def _state(self, _query: Query) -> None:
        mode = SCEN["plug"]
        if mode == "none":
            STATE["plug"] = None
        elif mode == "bad":
            # The failure this exists to catch: commanded off, meter says full.
            STATE["plug"] = {"w": 43.5, "v": 120.9, "age_s": 3, "verdict": -1}
        else:
            speed = max(0, int(STATE["speed"]))
            base = [1.4, 2.5, 3.9, 4.9, 7.0, 7.6, 10.3, 12.8, 15.4, 20.3, 23.6, 30.8, 37.8]
            STATE["plug"] = {"w": base[min(speed, 12)], "v": 120.9, "age_s": 3, "verdict": 1}
        return self._json(200, STATE)

    def _device(self, _query: Query) -> None:
        return self._json(200, DEVICE)

    def _stats(self, _query: Query) -> None:
        return self._json(200, STATS)

    def _sensors(self, _query: Query) -> None:
        return self._json(
            200,
            {
                "ok": True,
                "temp_c": 23.86,
                "rh": 38.9,
                "hpa": 999.9,
                # The off-board chain, warmed up: raws plus live indices.
                "voc_raw": 30125,
                "nox_raw": 15810,
                "voc": 93,
                "nox": 1,
                # The on-board sensor reads warm and dry next to the regulator.
            },
        )

    def _events(self, _query: Query) -> None:
        now = int(time.time())
        body = "\n".join(
            f"{now - i * 30} {1000 - i * 30} health rssi=-63 heap=46724 drops=0" for i in range(30)
        )
        return self._send(200, body, "text/plain")

    def _history(self, query: Query) -> None:
        days = query.get("days", [None])[0]
        if days not in ("1", "7", "30", "60"):
            return self._json(400, {"error": "days must be 1, 7, 30 or 60"})
        if days != "1" and not (SCEN["card"] and SCEN["synced"]):
            return self._json(503, {"error": "sd card not mounted"})
        return self._json(200, history(int(days)))

    def _boots(self, query: Query) -> None:
        days = query.get("days", [None])[0]
        if days not in ("1", "7", "30", "60"):
            return self._json(400, {"error": "days must be 1, 7, 30 or 60"})
        if not (SCEN["card"] and SCEN["synced"]):
            return self._json(503, {"error": "sd card not mounted"})
        return self._json(200, {"boots": boots(int(days))})

    def _csv(self, query: Query) -> None:
        # Range and columns both mirror web_history.cpp exactly. They had
        # drifted -- 8 columns and a 30-day cap against the firmware's 13 and
        # 60 -- so the mock was certifying an export the device never emits.
        if "days" in query:
            raw = query["days"][0]
            if not raw.isdigit() or not (1 <= int(raw) <= 60):
                return self._json(400, {"error": "days must be 1-60"})
        body = (
            "epoch,temp_c,rh,hpa,outside_f,speed,batt_v,chg,"
            "watts,voc_raw,nox_raw,voc,nox\n"
            "1786425931,24.00,40.0,999.9,73.2,9,4.20,1,20.3,30125,17004,93,1\n"
        )
        return self._send(200, body, "text/csv")

    # --- writes -----------------------------------------------------------
    def _set(self, query: Query) -> None:
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
    CONFIG_KEYS: dict[str, tuple[str, Callable[[str], object]]] = {
        "gason": ("gas_on", lambda v: v not in ("0", "false")),
        "gasspd": ("gas_spd", int),
        "gasvoc": ("gas_voc", int),
        "ckwh": ("cost_kwh", float),
        "auto": ("auto", lambda v: v != "0"),
        "max": ("auto_max", int),
        "min": ("auto_min", int),
        "onf": ("on_f", float),
        "offf": ("off_f", float),
    }

    # The ranges handle_config() enforces. Without these the mock happily
    # stored auto_max=99 and gas_voc=99999 and echoed them back, so the
    # console could be dogfooded into states the device would never produce.
    CONFIG_RANGE = {
        "max": (1, 12),
        "min": (0, 12),
        "gasspd": (1, 12),
        "gasvoc": (100, 500),
        "ckwh": (0.01, 2.0),
        "onf": (0.5, 20.0),
        "offf": (0.0, 20.0),
    }

    def _config(self, query: Query) -> None:
        for arg, (key, cast) in self.CONFIG_KEYS.items():
            if arg not in query:
                continue
            try:
                value = cast(query[arg][0])
            except ValueError:
                return self._json(400, {"error": f"bad {arg}"})
            span = self.CONFIG_RANGE.get(arg)
            if span is not None and isinstance(value, (int, float)):
                lo, hi = span
                if not (lo <= value <= hi):
                    # Same shape as the firmware: out of range is ignored, not
                    # an error. See the report -- both sides should arguably
                    # 400 here, but the mock's job is to match what ships.
                    continue
            STATE[key] = value
        STATE["uptime_s"] += 1
        return self._json(200, STATE)

    def _display(self, _query: Query) -> None:
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
                # The fitted glass showed red (2026-08-12), so the mock stands
                # in for the TRICOLOR part and the console renders the red
                # accents by default, matching the device.
                "tricolor": True,
                "age_s": 42,
                "black": base64.b64encode(bytes(black.buf)).decode(),
                "red": base64.b64encode(bytes(red.buf)).decode(),
            },
        )

    def _display_refresh(self, _query: Query) -> None:
        return self._json(200, {"ok": True})

    # The firmware's OTA token default, read from config.h's FAN_OTA_TOKEN so
    # there is one source of truth and no second copy to drift (or to look
    # like a leaked credential to a scanner -- it is the committed public
    # default, by the operator's choice). Routes that must be exercisable END
    # TO END -- provisioning, and the update path -- accept exactly this value,
    # so a spec can drive the success path as well as the refusal. The older
    # destructive routes stay refuse-only below: a mock that can be asked to
    # "format the card" and says yes teaches nothing.
    DEFAULT_TOKEN = firmware_default_token()  # gitleaks:allow -- a call, not a value

    def _token_ok(self, query: Query) -> bool:
        return query.get("token", [""])[0] == self.DEFAULT_TOKEN

    # Every argument handle_provision() in net/web_provision.cpp accepts, in
    # the same order. tests/test_web_contract.py pins the two lists together.
    PROVISION_KEYS = (
        "ssid",
        "pass",
        "mqtt_host",
        "mqtt_port",
        "mqtt_user",
        "mqtt_pass",
        "lat",
        "lon",
    )

    def _provision(self, query: Query) -> None:
        if not self._token_ok(query):
            return self._json(403, {"error": "bad token"})
        given = {k: query[k][0] for k in self.PROVISION_KEYS if k in query}
        if not given:
            return self._json(400, {"error": "no fields"})
        caps = {
            "ssid": 32,
            "pass": 64,
            "mqtt_host": 64,
            "mqtt_user": 64,
            "mqtt_pass": 64,
            "lat": 16,
            "lon": 16,
        }
        for k, v in given.items():
            if k == "mqtt_port":
                if not v.isdigit() or not (1 <= int(v) <= 65535):
                    return self._json(400, {"error": "bad mqtt_port"})
            elif len(v) > caps[k] or (k == "ssid" and not v):
                return self._json(400, {"error": f"bad {k}"})
        # Mirror what the device would come back reporting after its reboot;
        # passwords are stored nowhere and echoed nowhere, same as the firmware.
        host, _, port = DEVICE["broker"].rpartition(":")
        DEVICE["ssid"] = given.get("ssid", DEVICE["ssid"])
        DEVICE["broker"] = f"{given.get('mqtt_host', host)}:{given.get('mqtt_port', port)}"
        for k in ("mqtt_user", "lat", "lon"):
            if k in given:
                DEVICE[k] = given[k]
        return self._json(200, {"ok": True, "note": "rebooting with the new credentials"})

    def _update(self, query: Query) -> None:
        # Drain the multipart body either way: HTTP/1.1 keep-alive means an
        # unread body becomes the next request's first bytes.
        length = int(self.headers.get("Content-Length") or 0)
        if length:
            self.rfile.read(length)
        if not self._token_ok(query):
            return self._json(403, {"error": "bad token"})
        # The board "reboots": a new boot, the other slot, and -- when the
        # ota_fw knob names a version -- that version, confirmed. Without the
        # knob it comes back on the SAME fw, which is what a rollback looks like.
        STATE["boots"] = int(STATE["boots"]) + 1
        STATE["slot"] = "ota_1" if STATE.get("slot") == "ota_0" else "ota_0"
        if SCEN["ota_fw"]:
            STATE["fw"] = SCEN["ota_fw"]
            STATE["confirmed"] = True
        return self._json(200, {"ok": True, "note": "rebooting into new slot"})

    def _needs_token(self, _query: Query) -> None:
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
        "/api/boots": _boots,
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
        "/api/provision": _provision,
        "/api/raw": _needs_token,
        "/api/restart": _needs_token,
        "/api/sdformat": _needs_token,
        "/api/sdpurge": _needs_token,
        "/update": _update,
    }


class SSE(BaseHTTPRequestHandler):
    """The live-state stream, mirrored from net/sse.cpp: the device serves the
    console on 80 and SSE on 8081, the mock serves the console on PORT and SSE
    on PORT+1 (api.ts derives the offset from location.port). Same CORS rule
    as the firmware: echo an allowlisted Origin, never *, and drop a foreign
    one without streaming a byte. One state frame per second; a client that
    goes away is just a closed socket, not an error worth logging."""

    def log_message(self, *args: object) -> None:
        pass

    def do_GET(self) -> None:
        origin = self.headers.get("Origin")
        if origin is not None and origin not in SELF_ORIGINS:
            self.close_connection = True
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        if origin:
            self.send_header("Access-Control-Allow-Origin", origin)
        self.end_headers()
        try:
            while not SCEN["down"]:  # /_die silences the stream like the device dying
                self.wfile.write(b"data: " + json.dumps(STATE).encode() + b"\n\n")
                self.wfile.flush()
                time.sleep(1.0)
        except OSError:
            pass  # client closed the tab; EventSource cleanup, not a failure


if __name__ == "__main__":
    # Loopback only, and plain HTTP by design: the device this stands in for
    # serves plain HTTP and cannot do otherwise. NOSONAR (S5332), matching the
    # decision already documented in scripts/deploy.sh.
    import threading

    try:
        _sse_srv = ThreadingHTTPServer(("127.0.0.1", PORT + 1), SSE)  # NOSONAR
        threading.Thread(target=_sse_srv.serve_forever, daemon=True).start()
    except OSError:
        # A neighbour owns PORT+1. The console's poll covers state anyway, so
        # say so and keep serving rather than refusing to start.
        print(f"port {PORT + 1} busy; live SSE stream disabled", file=_sys.stderr)
    ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()  # NOSONAR
