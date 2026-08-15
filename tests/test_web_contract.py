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
# ...or as the WK_/WN_ macros generated from types.ts by gen_wire_keys.py.
# The macro name IS the field name upper-cased (that is the generator's whole
# rule), so the reverse map is just .lower().
CPP_MACRO = re.compile(r"\bW[KN]_([A-Z0-9_]+)\b")
# Series emitted via a helper that takes the JSON key as its own argument:
#   write_series(tx, "name", ...)   write_ints(tx, "name", ...)
# The key never appears as a \"name\": literal in the calling function, so
# CPP_KEY alone cannot see it. (append_series is the pre-1.14.23 spelling; it
# is kept here so the pattern still matches if a writer is reverted.)
CPP_SERIES = re.compile(r'(?:append_series|write_series|write_ints|write_gas)\(\s*\w+,\s*"(\w+)"')
# Fixed-name writers, where the key is baked into the helper rather than
# passed: write_ts() always emits exactly "ts".
CPP_FIXED = {"write_ts": "ts", "write_ts_derived": "ts"}
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
    keys = set(CPP_KEY.findall(body)) | set(CPP_SERIES.findall(body))
    keys |= {m.lower() for m in CPP_MACRO.findall(body)}
    for helper, key in CPP_FIXED.items():
        if re.search(rf"\b{helper}\s*\(", body):
            keys.add(key)
    return keys


def ts_block(name: str) -> str:
    text = TYPES.read_text()
    m = re.search(rf"export (?:interface {name}\b[^{{]*{{|type {name}\s*=)", text)
    assert m, f"{name} missing from types.ts"
    # Interfaces end at the first column-0 brace. Type aliases end at the
    # first ';' outside every brace -- the Sensors union grew multi-line when
    # the air chain landed, so "ends at the newline" stopped being true.
    if "interface" in m.group(0):
        end = text.find("\n}", m.start())
        assert end != -1
        return text[m.start() : end]
    depth = 0
    for i in range(m.end(), len(text)):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        elif c == ";" and depth == 0:
            return text[m.start() : i]
    raise AssertionError(f"unterminated type alias {name}")


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
    # BatteryState and PlugState are nested under DeviceState (batt, plug) and
    # written by the same function, so all three interfaces mirror one writer.
    check("state_json", "DeviceState", "BatteryState", "PlugState")


def test_handle_device_matches_deviceinfo():
    check("handle_device", "DeviceInfo")


def test_handle_stats_matches_stats():
    check("handle_stats", "Stats")


def test_handle_sensors_matches_sensors():
    check("handle_sensors", "Sensors")


def test_handle_history_matches_history():
    # ApiError covers the 400 branch: days is required to be 1|7|30|60 and the
    # rejection body is part of the wire contract like any other response.
    # The series keys live in write_all_series (the shared emitter both
    # branches call); handle_history itself carries source/interval_s/ts and
    # the error bodies.
    wire = cpp_keys("handle_history") | cpp_keys("write_all_series")
    typed = ts_fields("History") | ts_fields("ApiError")
    assert wire == typed, (
        f"history wire/type mismatch: firmware-only {sorted(wire - typed)}, "
        f"types-only {sorted(typed - wire)}"
    )


def test_handle_boots_matches_boots():
    # The restart marks the charts hang their outage labels on. BootMark is
    # nested inside Boots (the `boots` array), so one writer mirrors both.
    check("handle_boots", "Boots", "BootMark", "ApiError")


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


def test_history_branches_agree():
    """Both /api/history branches must emit the SAME series.

    The 7/30-day path used to run through a different writer that emitted only
    temp_c/rh/hpa -- no timestamps and none of the other four series -- so
    changing the range silently dropped four of the seven lines and left the
    chart with no real time axis. types.ts cannot catch that on its own: it
    describes one shape, and check() above passes as long as SOME branch emits
    each field. This pins that every series is written on BOTH sides.
    """
    # The series list now lives in ONE place -- write_all_series -- and both
    # branches call it, which removes the drift risk structurally. The pin
    # becomes: every declared series appears exactly once in the shared
    # emitter, and handle_history calls that emitter on BOTH sides.
    emitter = find_function("write_all_series")
    series = [
        "temp_c", "rh", "hpa", "out_f", "batt_v", "spd", "chg",
        "watts", "voc_raw", "nox_raw", "voc", "nox",
    ]  # fmt: skip
    for name in series:
        # WN_<NAME>, since the emitter spells keys via the generated macros.
        n = len(re.findall(rf"\bWN_{name.upper()}\b", emitter))
        assert n == 1, (
            f"write_all_series writes '{name}' {n} time(s); expected exactly 1. "
            f"A series missing here is the 7/30-day regression for every range."
        )
    body = find_function("handle_history")
    calls = len(re.findall(r"\bwrite_all_series\s*\(", body))
    assert calls == 2, (
        f"handle_history calls write_all_series {calls} time(s); expected 2 "
        f"(the SD branch and the ring branch)."
    )
    # And both branches must anchor their rows in real time.
    assert re.search(r"\bwrite_ts\s*\(", body), "SD branch must emit per-row ts[]"
    assert re.search(r"\bwrite_ts_derived\s*\(", body), "ring branch must emit ts[]"
    # source tells the caller which store answered; both branches set it.
    assert (
        len(re.findall(r'\\"source\\":', body)) == 2
    ), "both branches must report which store answered"


def test_handle_sd_purge_matches_purgeresult():
    """The purge response is part of the wire contract like any other.

    It was the one endpoint whose fields nothing pinned, and it grew three of
    them (remaining, leftover, skipped_dirs) while being debugged live.
    """
    check("handle_sd_purge", "PurgeResult", "ApiError")


# --------------------------------------------------------- authenticated reads
#
# Almost every GET on this device is open on the LAN by design. The core-dump
# pair is the exception, and the reason is specific: a core dump is a snapshot
# of RAM at the moment of the fault, and RAM at that moment holds g_token, the
# WiFi PSK and the MQTT password. An unauthenticated read of it hands over the
# credential that guards every write, so the read escalates to full control of
# the board. Origin checks do not cover this -- they stop a browser on another
# site from FORGING a request, not a client that simply asks.
#
# tests/test_http_contract.py pins the behaviour against the mock; this pins the
# firmware itself, which is the thing that actually serves the bytes.
def test_core_dump_handlers_check_the_token():
    # The handlers moved to web_maint.cpp in the web.cpp split; scan wherever
    # they live so a future move breaks on "not found", not on a stale path.
    web = "\n".join(p.read_text() for p in (SRC / "net").glob("web*.cpp"))
    for handler in ("handle_crash", "handle_crash_raw", "handle_crash_erase"):
        m = re.search(r"(?:static )?void " + handler + r"\(\)\s*\{(.*?)\n\}", web, re.S)
        assert m, f"{handler} not found -- did it move or get renamed?"
        body = m.group(1)
        assert "crash_auth_ok()" in body or "token_ok(" in body or "guard_token(" in body, (
            f"{handler} serves core-dump data without checking the token. That "
            f"image contains g_token itself, so an open read escalates to full "
            f"control of the board."
        )


# ------------------------------------------------- the mock's config surface
#
# scripts/mock_device.py stands in for the device when dogfooding and in the
# Playwright suite. A config argument the firmware accepts but the mock does not
# is worse than a missing feature: the mock answers 200 and echoes its state
# back unchanged, so the console appears to apply a setting that went nowhere,
# and every test against the mock agrees with it.
#
# That is not hypothetical. `onf` and `offf` -- the auto-mode differential --
# were absent here, so the two steppers that set it had never been exercised end
# to end by anything at all.
def test_mock_accepts_every_config_arg_the_firmware_does():
    web = (SRC / "net" / "web.cpp").read_text()
    body = re.search(r"static void handle_config\(\)\s*\{(.*?)\n\}", web, re.S)
    assert body, "handle_config() not found -- did it move or get renamed?"
    firmware_args = set(re.findall(r'hasArg\("(\w+)"\)', body.group(1)))
    assert firmware_args, "handle_config() parses no arguments; the regex is stale"

    mock = (ROOT / "scripts" / "mock_device.py").read_text()
    keys = re.search(r"CONFIG_KEYS(?::[^=]*)? = \{(.*?)\n    \}", mock, re.S)
    assert keys, "CONFIG_KEYS not found in mock_device.py"
    mock_args = set(re.findall(r'"(\w+)":', keys.group(1)))

    # newtoken rotates the OTA credential and is gated by a separate `auth`
    # argument, not by the drawer. It is deliberately not a CONFIG_KEY: the mock
    # would have to hold and check a token to stand in for it honestly, and a
    # mock that hands out credentials is not a thing worth building. Named here
    # rather than filtered loosely, so a genuinely new arg still fails.
    NOT_A_SETTING = {"newtoken"}

    # The reverse direction matters just as much. A key the MOCK accepts but the
    # firmware does not lets a control pass its end-to-end test against a server
    # that happily applied it, while the real device drops the argument on the
    # floor -- the same silent no-op as `onf`, only discovered on hardware.
    extra = mock_args - firmware_args
    assert not extra, (
        f"mock_device.py accepts config args the firmware does not: {sorted(extra)}. "
        f"An E2E test can pass against the mock while the device ignores the setting."
    )

    missing = firmware_args - mock_args - NOT_A_SETTING
    assert not missing, (
        f"mock_device.py ignores config args the firmware accepts: {sorted(missing)}. "
        f"The mock will answer 200 and change nothing, so the console looks like it "
        f"applied the setting."
    )
