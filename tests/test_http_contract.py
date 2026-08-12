"""The HTTP contract, exercised end to end against scripts/mock_device.py.

Every assertion here was first checked by hand against the real device on
2026-08-11; this turns that session into a regression suite. It runs against
the mock, so it pins the CONTRACT rather than the firmware -- an on-hardware
pass is still what proves the firmware honours it. Passing here is necessary
and not sufficient, which is the same caveat written at the top of the mock.
"""

from __future__ import annotations

import json
from pathlib import Path
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

import pytest

ROOT = Path(__file__).resolve().parents[1]
MOCK = ROOT / "scripts" / "mock_device.py"
CONSOLE = ROOT / "web" / "dist" / "console.html"
PORT = 8099
BASE = f"http://127.0.0.1:{PORT}"


def _free(port: int) -> bool:
    with socket.socket() as s:
        return s.connect_ex(("127.0.0.1", port)) != 0


@pytest.fixture(scope="module")
def mock():
    if not CONSOLE.exists():
        pytest.skip("web/dist/console.html not built")
    if not _free(PORT):
        pytest.skip(f"port {PORT} already in use")
    proc = subprocess.Popen(
        [sys.executable, str(MOCK)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE
    )
    for _ in range(50):
        time.sleep(0.1)
        if not _free(PORT):
            break
    else:
        proc.terminate()
        pytest.fail("mock device did not start")
    yield proc
    proc.terminate()
    # communicate(), not wait() then read(): nothing drains the stderr pipe
    # while the tests run, so a chatty mock could fill the OS buffer, block on
    # write and hang the suite. kill() on timeout keeps a stubborn mock from
    # holding the port for every later run.
    try:
        _, raw = proc.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        _, raw = proc.communicate()
    err = (raw or b"").decode(errors="replace")
    # A traceback here means a request crashed a handler, which is a defect
    # even when the response looked fine.
    assert "Traceback" not in err, f"mock raised during the suite:\n{err[-2000:]}"


def req(path: str, method: str = "GET", origin: str | None = None):
    r = urllib.request.Request(BASE + path, method=method)
    if origin:
        r.add_header("Origin", origin)
    try:
        with urllib.request.urlopen(r, timeout=5) as f:
            return f.status, f.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def get_json(path: str):
    status, body = req(path)
    assert status == 200, f"{path} -> {status}"
    return json.loads(body)


def scen(**kw):
    q = "&".join(f"{k}={str(v).lower() if isinstance(v, bool) else v}" for k, v in kw.items())
    status, _ = req(f"/_scen?{q}")
    assert status == 200, f"scenario {q} rejected"


# ------------------------------------------------------------------- writes


@pytest.mark.parametrize("path", ["/api/set?speed=0", "/api/config?auto=0", "/api/raw?high_pct=0"])
def test_mutating_routes_refuse_GET(mock, path):
    """They answered GET until 1.14.23, so any page could fire them with <img>."""
    status, _ = req(path)
    assert status == 404, f"GET {path} should not reach a mutating handler"


@pytest.mark.parametrize("path", ["/api/set?speed=0", "/api/config?auto=0"])
def test_mutating_routes_refuse_a_foreign_origin(mock, path):
    """POST-only is not enough: a cross-origin form POSTs without a preflight."""
    status, body = req(path, "POST", origin="http://evil.example")
    assert status == 403
    assert b"cross-origin" in body


def test_lookalike_origins_are_refused(mock):
    for origin in (
        "http://127.0.0.1.evil.example",
        "http://localhost.evil.example",
        "null",
        "https://127.0.0.1:8099",
    ):
        status, _ = req("/api/set?speed=1", "POST", origin=origin)
        assert status == 403, f"{origin} should not be accepted as self"


def test_the_console_origin_is_accepted(mock):
    status, _ = req("/api/set?speed=4", "POST", origin=BASE)
    assert status == 200
    assert get_json("/api/state")["speed"] == 4


def test_a_non_browser_caller_with_no_origin_is_accepted(mock):
    """curl and deploy.sh send no Origin; rejecting them buys nothing."""
    status, _ = req("/api/set?speed=6", "POST")
    assert status == 200
    assert get_json("/api/state")["speed"] == 6


def test_out_of_range_speed_is_refused(mock):
    for bad in ("13", "-1", "abc", ""):
        status, _ = req(f"/api/set?speed={bad}", "POST")
        assert status == 400, f"speed={bad!r} should be refused"


# ------------------------------------------------------------------ history


@pytest.mark.parametrize("days", ["1", "7", "30"])
def test_history_returns_one_shape_for_every_range(mock, days):
    """The 7/30-day path used to omit ts and four of the seven series."""
    scen(card=True, synced=True)
    h = get_json(f"/api/history?days={days}")
    assert h["source"] in ("sd", "ring")
    series = ["ts", "temp_c", "rh", "hpa", "out_f", "batt_v", "spd", "chg"]
    for name in series:
        assert name in h, f"days={days} response is missing {name}"
    lengths = {len(h[name]) for name in series}
    assert (
        len(lengths) == 1
    ), f"days={days} series lengths disagree: { {n: len(h[n]) for n in series} }"


def test_history_timestamps_are_real_and_ascending(mock):
    h = get_json("/api/history?days=1")
    ts = h["ts"]
    # Before indexing: all() over an empty list is true, so an empty series
    # would slip past the ordering check and fail with an IndexError below,
    # hiding which part of the contract actually broke.
    assert ts, "the 24 h range must return at least one row"
    assert all(b > a for a, b in zip(ts, ts[1:])), "timestamps must increase"
    assert ts[0] > 1_600_000_000, "timestamps must be real epochs, not indices"


@pytest.mark.parametrize("days", ["0", "2", "90", "abc", ""])
def test_history_rejects_undocumented_ranges(mock, days):
    """A malformed question gets an error, not the ring wearing the label."""
    status, _ = req(f"/api/history?days={days}")
    assert status == 400


def test_history_without_days_is_refused(mock):
    status, _ = req("/api/history")
    assert status == 400


def test_a_range_the_card_cannot_answer_is_503_not_substituted_data(mock):
    """Serving 24 h of RAM under a 30-day request is the failure this prevents."""
    scen(card=False)
    try:
        for days in ("7", "30"):
            status, body = req(f"/api/history?days={days}")
            assert status == 503, f"days={days} with no card should be 503, got {status}"
            assert b"error" in body
    finally:
        scen(card=True)


def test_an_outage_is_visible_as_a_gap_in_the_timestamps(mock):
    scen(card=True, gap_at=50)
    try:
        h = get_json("/api/history?days=1")
        ts, step = h["ts"], h["interval_s"]
        gaps = [b - a for a, b in zip(ts, ts[1:]) if b - a > step * 1.5]
        assert gaps, "a dark stretch must remain visible in the timestamps"
    finally:
        scen(gap_at="none")


# ----------------------------------------------------------------- download


def test_download_csv_has_the_full_column_set(mock):
    status, body = req("/download.csv?days=30")
    assert status == 200
    header = body.decode().splitlines()[0]
    assert header == "epoch,temp_c,rh,hpa,outside_f,speed,batt_v,chg"


@pytest.mark.parametrize("days", ["0", "31", "abc"])
def test_download_csv_rejects_a_bad_range(mock, days):
    """It used to clamp silently and label the file with the wrong span."""
    status, _ = req(f"/download.csv?days={days}")
    assert status == 400


# --------------------------------------------------------------------- misc


def test_state_carries_everything_the_console_and_deploy_need(mock):
    s = get_json("/api/state")
    for field in (
        "fw",
        "confirmed",
        "last_reset",
        "sd_total_mb",
        "sd_free_mb",
        "drops",
        "boots",
        "uptime_s",
        "mqtt",
        "speed",
        "auto",
    ):
        assert field in s, f"/api/state is missing {field}"


def test_uptime_and_boots_allow_frame_ordering(mock):
    """The console orders frames by (boots, uptime_s); both must be present."""
    a = get_json("/api/state")
    req("/api/set?speed=2", "POST")
    b = get_json("/api/state")
    assert (b["boots"], b["uptime_s"]) >= (a["boots"], a["uptime_s"])


def test_token_guarded_routes_refuse_without_one(mock):
    for path in ("/api/restart", "/api/sdformat", "/api/sdpurge", "/update"):
        status, _ = req(path, "POST")
        assert status == 403, f"{path} must not act without a token"


def test_core_dump_reads_require_a_token(mock):
    """A read that hands over the credential guarding every write.

    A core dump is a snapshot of RAM at the fault, and RAM holds g_token, the
    WiFi PSK and the MQTT password. Serving it unauthenticated turns a read into
    full control of the board, so these are the one pair of GETs that are gated.
    Origin checks are no help: they stop a browser on another site from FORGING
    a request, not a client that simply asks.
    """
    for path in ("/api/crash", "/api/crash.bin"):
        status, _ = req(path)
        assert status == 403, f"{path} must not serve a RAM image without a token"


def test_unknown_routes_are_404_json(mock):
    status, body = req("/api/nope")
    assert status == 404
    assert json.loads(body)["error"] == "404"


def test_the_console_page_is_served(mock):
    status, body = req("/")
    assert status == 200
    assert body.lstrip().startswith(b"<!doctype html>")
