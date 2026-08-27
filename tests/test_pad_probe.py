"""The pad probe must not call Arduino's digitalRead().

Found by tools/fantape on its first run against the device: 2118 of 2892 lines
on the flight recorder were the Arduino core saying "IO 18 is not set as GPIO".
The pin is attached to LEDC, so the peripheral manager does not have it down as
a GPIO, and digitalRead() checks that on every call and logs when it fails --
into the very recorder we read to diagnose the fan.

The noise was the smaller half. Logging made each read cost about 1.5 ms, so
probe_pad's 30 ms loop managed twenty samples. Three 100 Hz periods sampled
twenty times measures duty to roughly +/-5 points, and log_window()'s MISMATCH
alarm triggers at 5 points -- the instrument's own error was the size of the
effect it exists to detect.

Kept as a test because the fix looks like a style preference. gpio_get_level()
is one line away from digitalRead(), reads identically, and someone tidying up
would put the familiar one back. Nothing else would notice: the build stays
green, the probe keeps returning a number, and the number is quietly wrong.
"""

from __future__ import annotations

import re

from conftest import ROOT

CONTROL = ROOT / "firmware" / "arduino" / "src" / "fan" / "control.cpp"


def strip_comments(text: str) -> str:
    """Drop // and /* */ so a cautionary comment cannot satisfy a search."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def probe_body() -> str:
    """The text of probe_pad(), from its opening brace to the matching close."""
    src = CONTROL.read_text()
    start = src.index("void probe_pad(")
    depth = 0
    for i in range(src.index("{", start), len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[start : i + 1]
    raise AssertionError("unbalanced braces in probe_pad()")


def test_the_probe_is_still_there_to_check() -> None:
    """Guards the guard: a rename must not turn this file into a silent pass."""
    assert "gpio_get_level" in probe_body()


def test_it_does_not_read_the_pad_through_arduino() -> None:
    body = strip_comments(probe_body())
    assert "digitalRead" not in body, (
        "digitalRead() logs 'not set as GPIO' on every call for an LEDC-attached "
        "pin -- it floods the flight recorder and slows the sample loop to ~20 "
        "reads per 30 ms. Use gpio_get_level()."
    )


def test_the_reason_is_recorded_next_to_the_code() -> None:
    """The fix is one identifier and reverts as easily as it applied, so the
    why has to live where someone about to change it will read it."""
    body = probe_body()
    assert "digitalRead" in body, "the comment explaining what NOT to use is gone"
    assert "peripheral manager" in body or "periman" in body


def test_the_input_buffer_is_still_enabled_first() -> None:
    """Reading the level does nothing useful unless FUN_IE is set: without it
    the register reads a disconnected input, not the pad."""
    body = strip_comments(probe_body())
    assert "FUN_IE" in body
    assert body.index("FUN_IE") < body.index("gpio_get_level")


def test_nothing_else_in_the_file_reads_the_pwm_pin_through_arduino() -> None:
    """The same trap, one function over."""
    src = strip_comments(CONTROL.read_text())
    for call in re.findall(r"digitalRead\s*\(([^)]*)\)", src):
        assert "FAN_PWM_PIN" not in call, f"digitalRead({call}) hits the LEDC-attached pad"
