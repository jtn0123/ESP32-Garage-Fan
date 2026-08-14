"""The fan line must be DRIVEN by boot, whatever speed NVS remembers.

On 2026-08-13 the board was rebooted (an OTA deploy) while the fan was at
speed 0. ``fan::restore()`` only called ``set_wave()`` for a saved speed > 0,
so the control GPIO was left floating -- the RMT was never even initialised.
The fan's side of the link pulls the line up, a solid-high line reads as full
power on this rig, and the fan ran while the panel, the console and
/api/state all said OFF. Auto mode could never correct it: it kept deciding
0, and ``apply(0)`` short-circuits when the speed already is 0.

An undriven control line is not "off"; it is "whatever the fan feels like".
This pins the invariant structurally: restore() must contain a set_wave call
at function top level -- outside every conditional -- so every boot path ends
with the pin driven.
"""

from pathlib import Path
import re

CONTROL = Path(__file__).resolve().parents[1] / "firmware/arduino/src/fan/control.cpp"


def _strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def _restore_body() -> str:
    src = _strip_comments(CONTROL.read_text())
    start = src.index("void restore(")
    depth = 0
    for i in range(src.index("{", start), len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[src.index("{", start) : i + 1]
    raise AssertionError("unbalanced braces in restore()")


def test_restore_drives_the_wave_outside_every_conditional():
    body = _restore_body()
    depth = 0
    unconditional = False
    for m in re.finditer(r"\{|\}|set_wave\s*\(", body):
        tok = m.group(0)
        if tok == "{":
            depth += 1
        elif tok == "}":
            depth -= 1
        elif depth == 1:  # function scope, not inside any if/for block
            unconditional = True
    assert unconditional, (
        "fan::restore() has no set_wave call at function scope; a reboot at "
        "speed 0 leaves the control line floating and the fan does whatever "
        "its pull-up says while everything reports OFF"
    )
