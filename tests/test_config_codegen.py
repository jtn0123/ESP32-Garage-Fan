import os
import re
import shutil
import subprocess
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(__file__))
GENERATED_CONFIG = os.path.join(ROOT, "firmware", "arduino", "src", "generated_config.h")


@pytest.fixture(autouse=True)
def preserve_generated_config():
    """Restore firmware/arduino/src/generated_config.h after each test.

    Unlike the other generators, gen_device_header.py has no --output flag, so the
    tests below have to run it in place and read the real header back. That leaves a
    header built from this test's environment behind, and gen_ui.py takes
    UI_FW_VERSION from it: the next build would stamp that value into the tracked
    web/sim/ui_generated.js and dirty the tree. The header is gitignored, so nothing
    else notices the swap.
    """
    saved = None
    if os.path.exists(GENERATED_CONFIG):
        saved = GENERATED_CONFIG + ".pytest-backup"
        shutil.copy2(GENERATED_CONFIG, saved)
    try:
        yield
    finally:
        if saved:
            shutil.move(saved, GENERATED_CONFIG)


def _run(cmd: list[str], env: dict | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, env=env)


def _read(path: str) -> str:
    with open(path, "r") as f:
        return f.read()


def test_layout_header_is_deterministic(tmp_path):
    # Ensure running the generator twice yields identical bytes. Write to
    # tmp_path: the repo's display_layout.h is owned by gen_ui.py (a different
    # format), so generating in place would dirty the working tree.
    repo_header = os.path.join(ROOT, "firmware", "arduino", "src", "display_layout.h")
    repo_header_before = _read(repo_header) if os.path.exists(repo_header) else None
    gen = [sys.executable, os.path.join(ROOT, "scripts", "gen_layout_header.py")]
    out1 = tmp_path / "display_layout_1.h"
    out2 = tmp_path / "display_layout_2.h"
    r1 = _run(gen + ["--out", str(out1)])
    assert r1.returncode == 0, r1.stdout + r1.stderr
    r2 = _run(gen + ["--out", str(out2)])
    assert r2.returncode == 0, r2.stdout + r2.stderr

    assert out1.read_text() == out2.read_text()

    # Regression guard: the redirected runs must not touch the repo's header
    if repo_header_before is not None:
        assert _read(repo_header) == repo_header_before


def _gen_device_header_with_env(env_overrides: dict[str, str]) -> str:
    # Run generator with a clean environment overriding only what we need
    base_env = os.environ.copy()
    base_env.update(env_overrides)
    r = _run([sys.executable, os.path.join(ROOT, "scripts", "gen_device_header.py")], env=base_env)
    assert r.returncode == 0, r.stdout + r.stderr
    hdr = os.path.join(ROOT, "firmware", "arduino", "src", "generated_config.h")
    return _read(hdr)


def _extract_define(text: str, name: str) -> str | None:
    # Robustly match a single #define NAME value line across environments
    m = re.search(rf"(?m)^#define\s+{re.escape(name)}\s+([^\r\n]+)\s*$", text)
    return m.group(1).strip() if m else None


def test_flash_modes_drive_wake_interval_define():
    # 3 minutes
    h3 = _gen_device_header_with_env({"WAKE_INTERVAL": "3m"})
    assert _extract_define(h3, "WAKE_INTERVAL_SEC") == "180"

    # 1 hour
    h1 = _gen_device_header_with_env({"WAKE_INTERVAL": "1h"})
    assert _extract_define(h1, "WAKE_INTERVAL_SEC") == "3600"

    # 2 hours
    h2 = _gen_device_header_with_env({"WAKE_INTERVAL": "2h"})
    assert _extract_define(h2, "WAKE_INTERVAL_SEC") == "7200"


def test_mqtt_subscribe_base_matches_device_yaml():
    # Ensure config/device.yaml.subscribe base matches generated_config.h MQTT_SUB_BASE
    # This prevents topic drift that would blank the Outside/weather blocks
    # Generate header fresh
    r = _run([sys.executable, os.path.join(ROOT, "scripts", "gen_device_header.py")])
    assert r.returncode == 0, r.stdout + r.stderr
    hdr = _read(os.path.join(ROOT, "firmware", "arduino", "src", "generated_config.h"))
    sub_base_define = _extract_define(hdr, "MQTT_SUB_BASE")
    assert sub_base_define is not None

    # Load device.yaml when present; otherwise use sample.yaml
    import yaml  # type: ignore

    dev_path = os.path.join(ROOT, "config", "device.yaml")
    if not os.path.exists(dev_path):
        dev_path = os.path.join(ROOT, "config", "device.sample.yaml")
    with open(dev_path, "r") as f:
        data = yaml.safe_load(f) or {}
    mqtt = data.get("mqtt") or {}
    base_topics = mqtt.get("base_topics") or {}
    sub_base_yaml = str(base_topics.get("subscribe", "")).strip()
    assert sub_base_yaml, "mqtt.base_topics.subscribe missing in device yaml"

    # The C header value is a quoted string; strip quotes for comparison
    sub_base_c = sub_base_define.strip().strip('"')
    assert sub_base_c == sub_base_yaml


def test_flash_mode_always_sets_no_sleep_flag(tmp_path):
    # Create a small harness that patches flash.run() to print env and exit without running pio
    harness = tmp_path / "capture_env.py"
    harness.write_text(
        (
            "import os, sys\n"
            # Compute project root by walking up until we see scripts/flash.py
            "_d=os.path.abspath(os.path.dirname(__file__))\n"
            "ROOT=None\n"
            "for _ in range(6):\n"
            "    cand=os.path.abspath(os.path.join(_d, '..', '..'))\n"
            "    if os.path.exists(os.path.join(cand,'scripts','flash.py')):\n"
            "        ROOT=cand; break\n"
            "    _d=cand\n"
            "if ROOT is None: ROOT=os.getcwd()\n"
            "sys.path.insert(0, ROOT)\n"
            "from scripts import flash\n"
            "def fake_run(cmd):\n"
            "    print('EXTRA_FLAGS=' + os.environ.get('EXTRA_FLAGS',''))\n"
            "    print('WAKE_INTERVAL=' + os.environ.get('WAKE_INTERVAL',''))\n"
            "    return 0\n"
            "flash.run = fake_run\n"
            "sys.argv = ['flash.py', '--mode', 'always', '--build-only']\n"
            "raise SystemExit(flash.main())\n"
        )
    )
    proc = subprocess.run([sys.executable, str(harness)], cwd=ROOT, capture_output=True, text=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    # Verify the always mode programs no-sleep via EXTRA_FLAGS
    lines = proc.stdout.strip().splitlines()
    env_map = {}
    for ln in lines:
        if "=" in ln:
            k, v = ln.split("=", 1)
            env_map[k.strip()] = v.strip()
    assert env_map.get("EXTRA_FLAGS", "").find("-DDEV_NO_SLEEP=1") >= 0
    # WAKE_INTERVAL should be empty in always mode
    assert env_map.get("WAKE_INTERVAL", "") == ""
