"""The real runner must execute every declared native environment."""

import os
from pathlib import Path
import re
import subprocess

from conftest import ROOT


def test_native_runner_discovers_every_environment(tmp_path: Path) -> None:
    fake = tmp_path / "pio"
    fake.write_text('#!/bin/sh\nprintf "%s\\n" "$3" >> "$NATIVE_RUN_LOG"\n')
    fake.chmod(0o755)
    log = tmp_path / "envs"
    subprocess.run(
        ["bash", str(ROOT / "scripts/test-native.sh")],
        env={**os.environ, "PATH": f"{tmp_path}:{os.environ['PATH']}", "NATIVE_RUN_LOG": str(log)},
        check=True,
        capture_output=True,
    )
    expected = set(
        re.findall(
            r"^\[env:(native[^]]*)\]", (ROOT / "firmware/arduino/platformio.ini").read_text(), re.M
        )
    )
    # pio receives: test -e <environment> (the third argument).
    assert set(log.read_text().splitlines()) == expected
