"""Exercise real firmware handlers; fake only the Arduino platform boundary."""

from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "tests/firmware_host"
SRC = ROOT / "firmware/arduino/src"


@pytest.fixture(scope="module")
def firmware_handlers(tmp_path_factory: pytest.TempPathFactory) -> Path:
    compiler = shutil.which("c++")
    assert compiler, "A C++ compiler is required for production-handler regressions"
    binary = tmp_path_factory.mktemp("firmware-handlers") / "handlers"
    subprocess.run(
        [
            compiler,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(HOST / "stubs"),
            "-I",
            str(SRC),
            str(HOST / "handlers.cpp"),
            str(SRC / "net/web_controls.cpp"),
            str(SRC / "net/web_provision.cpp"),
            str(SRC / "net/creds.cpp"),
            str(SRC / "fan/control.cpp"),
            "-o",
            str(binary),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    return binary


@pytest.mark.parametrize(
    "case",
    [
        "credential-length",
        "credential-port",
        "credential-storage",
        "credential-success",
        "credential-migration",
        "numeric-speed",
        "numeric-config",
        "limit-config",
        "numeric-raw",
        "actuator-write",
        "actuator-attach",
        "query-auth",
        "public-token",
        "origin",
        "token-rotation",
        "token-storage",
        "token-bootstrap",
        "token-bootstrap-failure",
    ],
)
def test_production_handler(firmware_handlers: Path, case: str) -> None:
    subprocess.run([str(firmware_handlers), case], check=True, capture_output=True, text=True)
