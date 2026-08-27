import os
import sys

from conftest import ROOT as _ROOT

ROOT = str(_ROOT)


def test_gen_header_emits_wifi_country_and_bssid_channel() -> None:
    # Simulate a device.yaml dict
    data = {
        "room_name": "Test",
        "wake_interval": "1h",
        "wifi": {
            "ssid": "SSID",
            "password": "PASS",
            "bssid": "aa:bb:cc:dd:ee:ff",
            "channel": 6,
            "country": "US",
        },
        "mqtt": {
            "host": "h",
            "port": 1883,
        },
    }

    # Use the internal helpers to build the header string without writing a file.
    # The script currently writes directly; we can simulate by monkeypatching environment
    # but here we call its main path indirectly by recreating the logic.
    # Simpler: write a temp YAML and run the script to generate header, then inspect.
    import subprocess
    import tempfile

    import yaml  # type: ignore

    with tempfile.TemporaryDirectory() as td:
        cfg_dir = os.path.join(td, "config")
        os.makedirs(cfg_dir, exist_ok=True)
        ypath = os.path.join(cfg_dir, "device.yaml")
        with open(ypath, "w") as f:
            yaml.safe_dump(data, f)

        # Copy script into temp and run to emit header to temp firmware dir
        scripts_src = os.path.join(ROOT, "scripts", "gen_device_header.py")
        scripts_dst = os.path.join(td, "scripts", "gen_device_header.py")
        os.makedirs(os.path.dirname(scripts_dst), exist_ok=True)
        with open(scripts_src, "r") as f:
            src = f.read()
        with open(scripts_dst, "w") as f:
            f.write(src)

        # Adjust PRJ root paths by running from the temp project similar layout
        firmware_dir = os.path.join(td, "firmware", "arduino", "src")
        os.makedirs(firmware_dir, exist_ok=True)

        # Execute script
        subprocess.check_call([sys.executable, scripts_dst], cwd=os.path.join(td, "scripts"))

        hdr = os.path.join(firmware_dir, "generated_config.h")
        with open(hdr, "r") as f:
            out = f.read()

        assert '#define WIFI_BSSID "aa:bb:cc:dd:ee:ff"' in out
        assert "#define WIFI_CHANNEL 6" in out
        assert '#define WIFI_COUNTRY "US"' in out
