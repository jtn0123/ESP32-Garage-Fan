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

# One logged sample every STEP seconds is what the card holds; kGraphMaxPts is
# how many rows the firmware will hand back for any window (config.h).
DAY_ROWS = 86400 // STEP
MAX_PTS = 288


def history(days: int = 1) -> Json:
    # Keyed on the knobs, the RANGE, and the current second: the rows carry
    # timestamps, so a cache that ignored the clock would freeze the chart's
    # right-hand edge, and one that ignored `days` served the 24 h window under
    # every range's label -- which is precisely the bug this mock existed to
    # catch and instead reproduced.
    global _hist_cache
    key = (tuple(sorted(SCEN.items())), days, int(time.time()))
    cached = _hist_cache
    if cached is not None and cached[0] == key:
        return cached[1]
    value = _history_uncached(days)
    _hist_cache = (key, value)
    return value


def _window(days: int) -> "tuple[int, int]":
    """Rows returned for `days`, and the seconds between two of them.

    web_history.cpp reads the card from `now - days*86400` and hands the result
    to sdcard::read_range, which DECIMATES with `stride = rows/max_pts + 1` to
    fit kGraphMaxPts. So a wider range is a coarser step over a longer window,
    never more rows: 24 h returns 288 rows 300 s apart, 7 d returns 252 rows
    2400 s apart, 60 d returns 284 rows ~5 h apart. The `rows` knob is what the
    card actually HOLDS -- a short card cannot fill any window.
    """
    # `rows=none` is the documented way to put the knob back to "full card"
    # (coerce_scen stores None); int(None) here crashed every history request
    # after that reset (found 2026-08-20, dogfooding the knobs themselves).
    rows_knob = SCEN["rows"] if SCEN["rows"] is not None else 60 * DAY_ROWS
    stored = min(int(rows_knob), days * DAY_ROWS)
    stride = stored // MAX_PTS + 1 if stored > MAX_PTS else 1
    return (stored + stride - 1) // stride, STEP * stride


def _day_fraction(t: int) -> float:
    """Where t falls in its own LOCAL day, 0..1.

    Local, not UTC: the console shades night from `new Date().getHours()` and
    labels the axis the same way, so a mock whose diurnal curve peaked at UTC
    noon would put the hottest hour of the garage inside the night band on any
    machine west of Greenwich.
    """
    lt = time.localtime(t)
    return (lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec) / 86400.0


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
    history(days)  # ensure the window this asks about has been built
    cutoff = time.time() - days * 86400
    return [b for b in _boots if float(b["ts"]) >= cutoff]


def _history_uncached(days: int) -> Json:  # NOSONAR -- the branches ARE the scenario knobs
    n, step = _window(days)
    end = int(time.time())
    ts: list[int] = []
    temp: list[float | None] = []
    rh: list[int] = []
    hpa: list[float | None] = []
    out: list[float | None] = []
    batt: list[float] = []
    spd: list[int] = []
    chg: list[int] = []
    t = end - (n - 1) * step
    for i in range(n):
        if SCEN["gap_at"] is not None and i == SCEN["gap_at"]:
            t += step * 40  # a dark stretch, always wide enough to read as one
        ts.append(t)
        t += step
        # The analog series are functions of TIME, not of row index. Keyed on
        # the index they aliased into noise the moment a range was decimated:
        # sin(i/20) is a 10 h cycle at 300 s per row and a 6-DAY one at the
        # 60-day step, so the long ranges drew a plausible-looking curve that
        # meant nothing. A diurnal curve samples correctly at every range and
        # lines up with the console's night shading.
        frac = _day_fraction(t)
        swing = math.sin(2 * math.pi * (frac - 0.375))  # peaks mid-afternoon
        # A little sensor wobble on top. Without it the curve is analytically
        # smooth and renders as a staircase of the CSV's one decimal, which
        # reads as a rendering artifact rather than as a reading; with it the
        # long ranges also decimate into scatter, the way real rows do.
        wobble = 0.12 * math.sin(i * 1.7)
        temp.append(round(24 + 2.0 * swing + wobble, 1))
        # Humidity runs opposite the temperature (same water, warmer air).
        rh.append(40 if SCEN["flat_rh"] else round(45 - 6 * swing + wobble))
        hpa.append(round(999.9 + math.cos(2 * math.pi * t / (3 * 86400)) * 0.4, 1))
        # The yard leads the garage by about an hour and swings wider -- which
        # is the whole differential story the hero and the band are drawing.
        out.append(
            None if i % 17 == 0 else round(73 + 4 * math.sin(2 * math.pi * (frac - 0.333)), 1)
        )
        batt.append(round(4.05 + 0.15 * swing, 2))
        spd.append(i % 10)
        chg.append(1 if swing > 0 else 0)
    # The watt meter and the air chain: watts follows the logged fan speed
    # through the baseline table; the SGP41 columns model a sensor that was
    # plugged in mid-history -- nulls (absent), then raws with index 0
    # (warming), then indices -- so the console's warm-up handling is
    # exercised by the default data set.
    base_w = [1.4, 2.5, 3.9, 4.9, 7.0, 7.6, 10.3, 12.8, 15.4, 20.3, 23.6, 30.8, 37.8]
    watts = [round(base_w[min(sp, 12)] + (i % 3) * 0.2, 1) for i, sp in enumerate(spd)]
    # The cycling profile's column: flips the meter confirmed per bucket. None
    # before the meter existed (the same era as the gas nulls), 0 on a steady
    # fan, and under the plug=cycling scenario the last two hours carry the
    # 2026-08-20 signature -- watts jumping between stopped and flat out with
    # a flip count on every row -- so the power row's red tint is reachable.
    pre_meter = max(1, n // 3)
    flips: list[int | None] = [None if i < pre_meter else 0 for i in range(n)]
    # The bucket's draw range. A steady fan's range is the sampling wobble;
    # a cycling one's spans stopped to flat out, which is the whole point of
    # recording a range instead of one snapshot per five minutes.
    w_min: list[float | None] = [
        None if i < pre_meter else round(w - 0.3, 1) for i, w in enumerate(watts)
    ]
    w_max: list[float | None] = [
        None if i < pre_meter else round(w + 0.3, 1) for i, w in enumerate(watts)
    ]
    if SCEN["plug"] == "cycling":
        for i in range(max(pre_meter, n - 24), n):
            flips[i] = 2 + (i % 5)
            watts[i] = 45.1 if i % 2 else 4.3
            w_min[i] = 4.3
            w_max[i] = 45.6
            spd[i] = 10
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
        # The device's SAMPLE cadence, not this response's row spacing -- the
        # firmware reports the same constant for every range ("interval_s is
        # now only a nominal hint for gap detection; ts[] carries the truth").
        # A mock that quietly reported the decimated step here would hide the
        # console bug where trusting it flags every row of a 7-day range as an
        # outage.
        "interval_s": STEP,
        "ts": ts,
        "watts": watts,
        "flips": flips,
        "w_min": w_min,
        "w_max": w_max,
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
