"""The boot-order invariant that a bricked board bought.

On 2026-08-11 a core-dump probe was added near the top of ``setup()``. The
deployed board's partition table (tinyuf2-partitions-4MB.csv) has no coredump
slot, so the probe hung. The task watchdog was armed ~35 lines further down and
never got the chance, so:

  * nothing rebooted the board (the watchdog was the only thing that would),
  * so ``ota_rollback`` never saw an unhealthy boot -- it counts *reboots* that
    fail to reach the broker, and a hang is not a reboot,
  * so the A/B rollback never fired, and recovery needed a USB cable.

The lesson is an ordering one, and ordering is exactly what silently rots. Every
statement executed before ``esp_task_wdt_add`` is unrecoverable over the air, so
this test holds that prologue to pin safe-states and the watchdog itself --
nothing that touches NVS, a bus, a driver, or the network.
"""

from pathlib import Path
import re

import pytest

MAIN = Path(__file__).resolve().parents[1] / "firmware/arduino/src/fan_controller_main.cpp"


def setup_body() -> str:
    """The text of setup(), from its opening brace to the matching close."""
    src = MAIN.read_text()
    start = src.index("void setup() {")
    depth = 0
    for i in range(src.index("{", start), len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[start : i + 1]
    raise AssertionError("unbalanced braces in setup()")


def strip_comments(text: str) -> str:
    """Drop // and /* */ so a cautionary comment cannot satisfy a search."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def prologue() -> str:
    """Everything that runs before the watchdog is armed."""
    body = strip_comments(setup_body())
    idx = body.index("esp_task_wdt_add")
    return body[:idx]


def test_watchdog_is_armed_in_setup():
    body = strip_comments(setup_body())
    assert "esp_task_wdt_init" in body, "setup() must arm the task watchdog"
    assert "esp_task_wdt_add(NULL)" in body, "the loop task must be watched"


def test_the_watchdog_is_configured_before_the_task_registers():
    """init must precede add, or the prologue check measures nothing.

    Every other test here slices the body at ``esp_task_wdt_add``. If add moved
    above init, that slice would still look tidy while the watchdog was never
    actually configured -- the checks would pass and the board would be exactly
    as brickable as before.
    """
    body = strip_comments(setup_body())
    assert body.index("esp_task_wdt_init") < body.index("esp_task_wdt_add"), (
        "esp_task_wdt_add runs before esp_task_wdt_init, so the task registers "
        "against an unconfigured watchdog and nothing enforces a timeout."
    )


# The specific calls that hung. Each one reaches flash or a driver, and each one
# sat *above* the watchdog on the build that bricked.
@pytest.mark.parametrize(
    "call",
    [
        "ota_rollback_check_at_boot",  # NVS reads
        "g_prefs.begin",  # NVS namespace open
        "battery::restore",
        "climate::restore",
        "fan::restore",
        "odometer::restore",
        "crashlog::examine_boot",
        "crumb_ring::render",
        "Wire.begin",  # I2C -- a stuck bus hangs here
        "SPI.begin",
        "display::begin",
        "wifi_link::begin",
        "mqtt_link::init",
        "web::begin",
    ],
)
def test_hangable_calls_run_under_the_watchdog(call):
    body = strip_comments(setup_body())
    if call not in body:
        pytest.skip(f"{call} is no longer in setup()")
    assert call not in prologue(), (
        f"{call} runs BEFORE esp_task_wdt_add, so a hang inside it cannot "
        f"reboot the board and ota_rollback can never recover it. Move it "
        f"below the watchdog block."
    )


def test_coredump_probe_is_never_in_the_boot_prologue():
    """The exact call that bricked the board. It must not creep back up."""
    pro = prologue()
    for needle in ("coredump", "esp_core_dump", "esp_partition_find"):
        assert needle not in pro, (
            f"{needle!r} is back in the pre-watchdog prologue -- this is the "
            f"2026-08-11 brick verbatim."
        )


def test_prologue_is_short_and_only_pin_safe_states():
    """A tight budget: the fan's safe-state pins, Serial, and the watchdog.

    Asserting on a line count is crude, but the failure mode here is drift --
    one more 'harmless' call at a time -- and a number is what makes drift
    visible in review.
    """
    lines = [ln.strip() for ln in prologue().splitlines() if ln.strip()]
    # 9 today: the brace, four pin statements, Serial.begin, and the three
    # lines of the watchdog block itself.
    assert len(lines) <= 9, "the pre-watchdog prologue is growing:\n" + "\n".join(lines)
    allowed = ("void setup()", "pinMode", "digitalWrite", "Serial.begin")
    for ln in lines:
        # The watchdog block itself is the point of the prologue, and one of its
        # lines is an `if (esp_task_wdt_init(...))` guard, so match anywhere.
        assert ln.startswith(allowed) or "esp_task_wdt" in ln, (
            f"unexpected statement before the watchdog is armed: {ln!r}. "
            f"Anything here is unrecoverable over the air."
        )
