"""Tests for the ALWAYS_ON sampling cadence resolved by gen_device_header.py.

Imported and called in-process rather than run as a subprocess: coverage.py does
not follow subprocesses, so the subprocess-based generator tests in
test_config_codegen.py exercise this code without recording it. These also avoid
touching the repo's generated_config.h, since resolve_sample_interval() is pure.

The bounds mirror RC_MIN/MAX_SAMPLE_INTERVAL_SEC in
firmware/arduino/src/runtime_config.h. Below the minimum the BME280 self-heats
and reads high, which is why an out-of-range value is rejected rather than used.
"""

import os
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(__file__))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

import gen_device_header as gdh  # noqa: E402


class TestResolveSampleInterval:
    def test_defaults_to_five_minutes(self):
        assert gdh.resolve_sample_interval({}, env={}) == 300

    def test_reads_yaml_value(self):
        assert gdh.resolve_sample_interval({"sample_interval": "10m"}, env={}) == 600

    def test_accepts_plain_seconds(self):
        assert gdh.resolve_sample_interval({"sample_interval": "90"}, env={}) == 90

    def test_env_overrides_yaml(self):
        data = {"sample_interval": "10m"}
        assert gdh.resolve_sample_interval(data, env={"SAMPLE_INTERVAL": "2m"}) == 120

    def test_blank_env_falls_back_to_yaml(self):
        data = {"sample_interval": "7m"}
        assert gdh.resolve_sample_interval(data, env={"SAMPLE_INTERVAL": "  "}) == 420

    @pytest.mark.parametrize("value", ["5", "0", "59"])
    def test_rejects_below_minimum(self, value, capsys):
        # Too-frequent sampling self-heats the sensor, so this must not pass through.
        assert gdh.resolve_sample_interval({"sample_interval": value}, env={}) == 300
        assert "outside" in capsys.readouterr().out

    @pytest.mark.parametrize("value", ["2h", "86400"])
    def test_rejects_above_maximum(self, value, capsys):
        assert gdh.resolve_sample_interval({"sample_interval": value}, env={}) == 300
        assert "outside" in capsys.readouterr().out

    def test_rejects_out_of_range_env_value(self, capsys):
        assert gdh.resolve_sample_interval({}, env={"SAMPLE_INTERVAL": "1s"}) == 300
        assert "outside" in capsys.readouterr().out

    @pytest.mark.parametrize("value", ["60", "3600"])
    def test_bounds_are_inclusive(self, value, capsys):
        assert gdh.resolve_sample_interval({"sample_interval": value}, env={}) == int(value)
        assert "outside" not in capsys.readouterr().out

    def test_falls_back_to_process_environment(self, monkeypatch):
        """env=None is the production call from main(); it must read os.environ."""
        monkeypatch.setenv("SAMPLE_INTERVAL", "4m")
        assert gdh.resolve_sample_interval({"sample_interval": "10m"}) == 240

        monkeypatch.delenv("SAMPLE_INTERVAL", raising=False)
        assert gdh.resolve_sample_interval({"sample_interval": "10m"}) == 600

    def test_bounds_match_firmware_constants(self):
        """The generator and runtime_config.h must agree, or a value accepted at
        build time could still be rejected on the device."""
        header = os.path.join(ROOT, "firmware", "arduino", "src", "runtime_config.h")
        with open(header, "r") as f:
            text = f.read()
        assert f"#define RC_MIN_SAMPLE_INTERVAL_SEC {gdh.MIN_SAMPLE_INTERVAL_SEC}" in text
        assert f"#define RC_MAX_SAMPLE_INTERVAL_SEC {gdh.MAX_SAMPLE_INTERVAL_SEC}" in text
        assert f"#define RC_DEFAULT_SAMPLE_INTERVAL_SEC {gdh.DEFAULT_SAMPLE_INTERVAL_SEC}" in text
