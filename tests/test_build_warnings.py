"""The device builds must keep the warning set that -isystem made possible.

Why this is a test and not just a line in platformio.ini: the flags below are
only clean because scripts/vendor_includes_as_system.py re-offers third-party
include directories as -isystem. Remove that script and the build still
succeeds -- it just buries our warnings under a few hundred from headers we do
not own, and the next real one scrolls past unread. The two have to move
together, and nothing else checks that they do.

Cheap on purpose: this parses the ini rather than building anything, so it runs
in the Python job rather than adding minutes to the firmware one. CI's actual
`-Werror` build is what proves the flags still pass.
"""

from __future__ import annotations

import configparser

from conftest import ROOT
import pytest

PIO_INI = ROOT / "firmware" / "arduino" / "platformio.ini"

# Each earns its place in the note in platformio.ini; each was drowned by
# vendored headers before the -isystem change.
REQUIRED = (
    "-Wall",
    "-Wextra",
    "-Wshadow",
    "-Wredundant-decls",
    "-Wextra-semi",
    "-Wcast-qual",
    "-Wnon-virtual-dtor",
)

VENDOR_SCRIPT = "vendor_includes_as_system.py"


def _config() -> configparser.ConfigParser:
    cfg = configparser.ConfigParser(inline_comment_prefixes=(";",))
    cfg.read(PIO_INI)
    return cfg


def _device_envs(cfg: configparser.ConfigParser) -> list[str]:
    """Every env that builds for the board, i.e. not the host test envs."""
    return [s for s in cfg.sections() if s.startswith("env:") and "native" not in s]


def test_there_are_device_envs_to_check() -> None:
    """Guards the guard: a rename that empties the list must not read as a pass."""
    assert len(_device_envs(_config())) >= 2


@pytest.mark.parametrize("flag", REQUIRED)
def test_every_device_env_carries_the_flag(flag: str) -> None:
    cfg = _config()
    for env in _device_envs(cfg):
        flags = cfg[env].get("build_src_flags", "")
        assert flag in flags.split(), f"{env} lost {flag}"


def test_every_device_env_runs_the_vendor_isystem_script() -> None:
    """Without it the flags above report a few hundred warnings from Adafruit
    EPD and the Arduino Network library, and ours are lost in them."""
    cfg = _config()
    for env in _device_envs(cfg):
        scripts = cfg[env].get("extra_scripts", "")
        assert VENDOR_SCRIPT in scripts, f"{env} does not run {VENDOR_SCRIPT}"


def test_the_native_envs_carry_the_same_set() -> None:
    """They compile the pure headers, which is where most of this logic lives."""
    cfg = _config()
    flags = cfg["native_base"].get("build_flags", "").split()
    for flag in REQUIRED:
        assert flag in flags, f"native_base lost {flag}"
