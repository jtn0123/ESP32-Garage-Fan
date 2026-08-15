"""The mock's data plane: the shipped console bytes and the synthetic
history that exercises gaps, flat lines and near-empty cards. Split from
mock_device.py at the 500-line ceiling."""

from __future__ import annotations

import math
from pathlib import Path
import time

from mock_state import SCEN, STEP, Json

# Serve the console straight out of the build tree, so dogfooding always
# exercises the bundle that would actually ship.
CONSOLE = Path(__file__).resolve().parents[1] / "web" / "dist" / "console.html"

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


_hist_cache: "tuple[object, Json] | None" = None


def history() -> Json:
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


# Restart marks for the window the last history build produced. Derived there
# rather than independently so the mark always lands inside the mock's gap --
# a marker that missed its own outage would make the console look broken while
# the console was right.
_boots: list[Json] = []


def boots(days: int = 60) -> list[Json]:
    """Marks inside the window, like bootlog::stream's cutoff.

    It used to ignore `days` entirely and hand back everything, so the mock
    would have certified a console that ignored the selected range.
    """
    history()  # ensure the current scenario has been built
    cutoff = time.time() - days * 86400
    return [b for b in _boots if float(b["ts"]) >= cutoff]


def _history_uncached() -> Json:  # NOSONAR -- the branches ARE the scenario knobs
    n = int(SCEN["rows"])
    end = int(time.time())
    ts: list[int] = []
    temp: list[float | None] = []
    rh: list[int] = []
    hpa: list[float | None] = []
    out: list[float | None] = []
    batt: list[float] = []
    spd: list[int] = []
    chg: list[int] = []
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
    # The watt meter and the air chain: watts follows the logged fan speed
    # through the baseline table; the SGP41 columns model a sensor that was
    # plugged in mid-history -- nulls (absent), then raws with index 0
    # (warming), then indices -- so the console's warm-up handling is
    # exercised by the default data set.
    base_w = [1.4, 2.5, 3.9, 4.9, 7.0, 7.6, 10.3, 12.8, 15.4, 20.3, 23.6, 30.8, 37.8]
    watts = [round(base_w[min(sp, 12)] + (i % 3) * 0.2, 1) for i, sp in enumerate(spd)]
    vocr: list[int | None] = []
    noxr: list[int | None] = []
    voc: list[int | None] = []
    nox: list[int | None] = []
    third = max(1, n // 3)
    for i in range(n):
        if i < third:
            vocr.append(None)
            noxr.append(None)
            voc.append(None)
            nox.append(None)
        elif i < 2 * third:
            vocr.append(29000 + (i % 40) * 20)
            noxr.append(15600 + (i % 25) * 8)
            voc.append(0)
            nox.append(0)
        else:
            vocr.append(30000 + (i % 40) * 20)
            noxr.append(15800 + (i % 25) * 8)
            voc.append(80 + (i % 30))
            nox.append(1 + (i % 3))
    # The restart that EXPLAINS the gap: stamped inside it, the way the real
    # device records a boot that happened between two samples.
    global _boots
    _boots = []
    if SCEN["gap_at"] is not None and 0 < SCEN["gap_at"] < len(ts):
        i = int(SCEN["gap_at"])
        _boots = [
            {
                "ts": (ts[i - 1] + ts[i]) // 2,
                "n": 82,
                "cause": "brownout",
            }
        ]
    if SCEN["corrupt"]:  # what a bad CSV row must become
        if len(temp) > 5:
            temp[5] = None
        if len(hpa) > 7:
            hpa[7] = None
    return {
        "source": "sd" if SCEN["card"] else "ring",
        "interval_s": STEP,
        "ts": ts,
        "watts": watts,
        "voc_raw": vocr,
        "nox_raw": noxr,
        "voc": voc,
        "nox": nox,
        "temp_c": temp,
        "rh": rh,
        "hpa": hpa,
        "out_f": out,
        "batt_v": batt,
        "spd": spd,
        "chg": chg,
    }
