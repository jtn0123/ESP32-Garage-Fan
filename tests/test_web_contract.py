"""The wire contract between the firmware's JSON writers and web/src/types.ts.

The firmware emits JSON with hand-rolled snprintf and the console reads it
through the interfaces in types.ts. Nothing else keeps the two in step: rename
a field on either side and the UI silently degrades to a dash. This test makes
that a CI failure instead.

Both sides are parsed from source, so the test keeps working as the module
split moves the C++ functions between files -- it finds each function wherever
it lives under firmware/arduino/src.
"""

from __future__ import annotations

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "firmware" / "arduino" / "src"
TYPES = ROOT / "web" / "src" / "types.ts"

# JSON keys as they appear inside C++ string literals: \"key\":
CPP_KEY = re.compile(r'\\"([A-Za-z_]\w*)\\":')
# Series appended via the helper: append_series(out, "name", ...)
CPP_SERIES = re.compile(r'append_series\(\s*\w+,\s*"(\w+)"')
# TS interface fields:   name: T;   name?: T;   'quoted' names are not used.
TS_FIELD = re.compile(r"^\s*(\w+)\??:", re.MULTILINE)


def cpp_sources() -> list[Path]:
    files = [p for p in SRC.rglob("*.cpp") if "generated_" not in p.name]
    assert files, f"no C++ sources under {SRC}"
    return files


def find_function(name: str) -> str:
    """Body of a top-level function, located in whichever file defines it.

    Relies on the repo's clang-format style: the definition opens on the line
    naming the function and the matching brace is the next '}' at column 0.
    """
    sig = re.compile(rf"^(?:static\s+)?[\w:<>*&\s]+\b{name}\s*\(", re.MULTILINE)
    for path in cpp_sources():
        text = path.read_text()
        m = sig.search(text)
        if not m:
            continue
        end = text.find("\n}\n", m.start())
        assert end != -1, f"unterminated body for {name} in {path}"
        return text[m.start() : end]
    raise AssertionError(f"function {name}() not found anywhere under {SRC}")


def cpp_keys(function: str) -> set[str]:
    body = find_function(function)
    return set(CPP_KEY.findall(body)) | set(CPP_SERIES.findall(body))


def ts_block(name: str) -> str:
    text = TYPES.read_text()
    m = re.search(rf"export (?:interface {name}\b[^{{]*{{|type {name}\s*=)", text)
    assert m, f"{name} missing from types.ts"
    # Interfaces end at the first column-0 brace. Type aliases in this file
    # are single-line unions whose object members use ';' internally, so the
    # alias ends at the newline, not at the first semicolon.
    if "interface" in m.group(0):
        end = text.find("\n}", m.start())
    else:
        end = text.find("\n", m.start())
    assert end != -1
    return text[m.start() : end]


def ts_fields(name: str) -> set[str]:
    block = ts_block(name)
    if block.startswith("export type"):
        # Type aliases (the Sensors union) are single expressions, not one
        # field per line -- take every `name:` inside the braces instead.
        return set(re.findall(r"(\w+)\??\s*:", block.split("=", 1)[1]))
    return set(TS_FIELD.findall(block))


def check(function: str, *ts_names: str) -> None:
    wire = cpp_keys(function)
    typed = set()
    for n in ts_names:
        typed |= ts_fields(n)
    missing_in_ts = wire - typed
    missing_in_cpp = typed - wire
    assert not missing_in_ts, (
        f"{function}() emits {sorted(missing_in_ts)} but types.ts does not "
        f"declare them -- add the fields (the console cannot see them otherwise)"
    )
    assert not missing_in_cpp, (
        f"types.ts declares {sorted(missing_in_cpp)} but {function}() does not "
        f"emit them -- the console reads fields the firmware never sends"
    )


def test_state_json_matches_devicestate():
    # BatteryState is nested under DeviceState.batt and written by the same
    # function, so both interfaces mirror one writer.
    check("state_json", "DeviceState", "BatteryState")


def test_handle_device_matches_deviceinfo():
    check("handle_device", "DeviceInfo")


def test_handle_stats_matches_stats():
    check("handle_stats", "Stats")


def test_handle_sensors_matches_sensors():
    check("handle_sensors", "Sensors")


def test_handle_history_matches_history():
    # ApiError covers the 400 branch: days is required to be 1|7|30 and the
    # rejection body is part of the wire contract like any other response.
    check("handle_history", "History", "ApiError")


def test_duty_table_matches_protocol():
    """kHighUs is measured off the wall controller; the console renders it via
    /api/device. Pin the values so neither side can be 'tidied' silently."""
    expected = [0, 3477, 4072, 4868, 5066, 5661, 6159, 6754, 7251, 7847, 8344, 8940, 9437]
    for path in [SRC / "config.h"]:
        text = path.read_text()
        m = re.search(r"kHighUs\[13\]\s*=\s*\{([^}]+)\}", text)
        assert m, f"kHighUs not found in {path}"
        values = [int(v) for v in re.findall(r"\d+", m.group(1))]
        assert values == expected, "the measured duty table changed -- PROTOCOL.md is ground truth"
