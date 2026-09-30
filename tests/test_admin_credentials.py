"""Build defaults and deployment must fail closed, without real credentials."""

import os
from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize(
    "token", ["", "iliving-ota", "pick_a_long_random_token", "short", "x" * 39, "bad\nheader"]
)
def test_public_or_invalid_token_is_not_compiled(tmp_path: Path, token: str) -> None:
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (tmp_path / "config").mkdir()
    (tmp_path / "firmware/arduino/src").mkdir(parents=True)
    generator = scripts / "gen_device_header.py"
    shutil.copyfile(ROOT / "scripts/gen_device_header.py", generator)
    subprocess.run(
        [sys.executable, str(generator)],
        cwd=tmp_path,
        env={"PATH": os.environ["PATH"], "FAN_OTA_TOKEN": token},
        check=True,
        capture_output=True,
        text=True,
    )
    assert (
        "#define FAN_OTA_TOKEN"
        not in (tmp_path / "firmware/arduino/src/generated_config.h").read_text()
    )


def test_deploy_uses_newly_built_token_in_header(tmp_path: Path) -> None:
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    binary = tmp_path / "bin"
    binary.mkdir()
    src = tmp_path / "firmware/arduino/src"
    src.mkdir(parents=True)
    (src / "generated_config.h").write_text('#define FAN_OTA_TOKEN "stale-test-token"\n')
    shutil.copyfile(ROOT / "scripts/deploy.sh", scripts / "deploy.sh")
    pio = binary / "pio"
    pio.write_text("""#!/usr/bin/env bash
set -eu
mkdir -p "$TEST_ROOT/firmware/arduino/.pio/build/feather_esp32s2_fan_controller"
image_dir="$TEST_ROOT/firmware/arduino/.pio/build/feather_esp32s2_fan_controller"
printf 'fake image' > "$image_dir/firmware.bin"
cat > "$TEST_ROOT/firmware/arduino/src/generated_config.h" <<'HEADER'
#define FAN_OTA_TOKEN "new-test-only-token"
#define WIFI_SSID "test-net"
#define FW_VERSION "1.0.0"
HEADER
""")
    curl = binary / "curl"
    curl.write_text("""#!/usr/bin/env bash
set -eu
case "${!#}" in
  http://test-host/update)
    for arg in "$@"; do
      if [[ "$arg" = @* ]]; then
        grep -qx 'X-Fan-Token: new-test-only-token' "${arg#@}"
        printf '%s' "${arg#@}" > "$TEST_ROOT/auth-path"
      fi
    done
    test -s "$TEST_ROOT/auth-path"
    printf 200
    ;;
  http://test-host/api/state)
    printf '%s' '{"fw":"1.0.0","confirmed":true,"last_reset":"sw_reset","sd_total_mb":1,"drops":0}'
    ;;
  *) exit 2 ;;
esac
""")
    pio.chmod(0o755)
    curl.chmod(0o755)
    result = subprocess.run(
        ["bash", str(scripts / "deploy.sh"), "test-host"],
        env={"PATH": f"{binary}:{os.environ['PATH']}", "TEST_ROOT": str(tmp_path)},
        check=True,
        capture_output=True,
        text=True,
    )
    assert "VERIFIED:" in result.stdout
    assert "new-test-only-token" not in result.stdout
    assert not Path((tmp_path / "auth-path").read_text()).exists()
