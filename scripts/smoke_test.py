#!/usr/bin/env python3
"""Hardware smoke test for a room node on USB.

Drives the always-on serial console (status/batt/hist/storage) and does one
MQTT command round-trip, asserting the answers. Thirty seconds of this would
have caught every device-side bug found in the v2.01 debugging session: the
starved graph ring, the latched-off fuel gauge, and the up-but-unreachable
link. Run it whenever the device is on a cable:

    python scripts/smoke_test.py                 # auto-detect port, .env creds
    python scripts/smoke_test.py --port /dev/cu.usbmodem01
    python scripts/smoke_test.py --skip-mqtt     # serial-only (no broker here)

Checks that depend on optional hardware degrade to WARN, not FAIL: a
disconnected battery pack or a pulled SD card should not fail CI for the
subsystems that are working. Exit code is non-zero only on FAIL.

Requires pyserial for the serial half (PlatformIO's penv has it) and the
mosquitto clients for the MQTT half (brew install mosquitto).
"""

import argparse
import glob
import json
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]

PASS, WARN, FAIL = "PASS", "WARN", "FAIL"
RESULTS: list[tuple[str, str, str]] = []


def record(level: str, name: str, detail: str = "") -> None:
    RESULTS.append((level, name, detail))
    print(f"  [{level}] {name}{('  — ' + detail) if detail else ''}")


def load_dotenv() -> dict:
    env = {}
    path = ROOT / ".env"
    if not path.exists():
        # Worktrees have no .env of their own; the credentials live at the main
        # checkout. git-common-dir points there from any worktree.
        try:
            common = subprocess.run(
                ["git", "-C", str(ROOT), "rev-parse", "--path-format=absolute", "--git-common-dir"],
                capture_output=True,
                text=True,
                timeout=10,
            ).stdout.strip()
            if common:
                path = Path(common).parent / ".env"
        except (subprocess.TimeoutExpired, FileNotFoundError):
            pass
    if not path.exists():
        return env
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        env[k.strip()] = v.strip().strip('"').strip("'")
    return env


def autodetect_port() -> str | None:
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    return ports[0] if ports else None


def serial_console(port: str) -> dict:
    """Send each console command, return {cmd: response_text}."""
    import serial  # deferred so --skip-serial-less runs don't need pyserial

    p = serial.Serial()
    p.port = port
    p.baudrate = 115200
    p.timeout = 1
    p.dtr = True  # USB-CDC transmits only once the host raises DTR
    p.rts = False  # never toggle both: that can trip ROM download mode
    p.open()
    time.sleep(2)
    p.reset_input_buffer()

    out: dict[str, str] = {}
    for cmd, wait in [("status", 2.5), ("batt", 3.0), ("hist", 2.0), ("storage", 10.0)]:
        p.write((cmd + "\n").encode())
        p.flush()
        time.sleep(wait)
        buf = b""
        while p.in_waiting:
            buf += p.read(p.in_waiting)
            time.sleep(0.2)
        out[cmd] = buf.decode("utf-8", "replace")
    p.close()
    return out


def check_serial(responses: dict) -> str | None:
    """Assert on console output. Returns the device client hint (ip) if seen."""
    status = responses.get("status", "")
    m = re.search(r"wifi_connected=(\d) ip=([0-9.]+) rssi=(-?\d+) mqtt_connected=(\d)", status)
    if not m:
        record(FAIL, "serial console answers", f"no status line in: {status[:120]!r}")
        return None
    record(PASS, "serial console answers")

    wifi, ip, _rssi, mqtt = m.group(1), m.group(2), m.group(3), m.group(4)
    record(PASS if wifi == "1" else FAIL, "wifi connected", f"wifi_connected={wifi}")
    record(PASS if ip not in ("0.0.0.0", "") else FAIL, "has an IP", ip)
    record(PASS if mqtt == "1" else FAIL, "broker connected", f"mqtt_connected={mqtt}")

    fw = re.search(r"fw=(\S+)", status)
    record(PASS if fw else FAIL, "firmware version reported", fw.group(1) if fw else "missing")

    rb = re.search(r"image_confirmed=(\d) unhealthy_boots=(\d+)", status)
    if rb:
        confirmed, boots = rb.group(1), int(rb.group(2))
        # Unconfirmed is legitimate for the first minutes after a fresh flash.
        record(
            PASS if confirmed == "1" or boots < 3 else FAIL,
            "boot health",
            f"image_confirmed={confirmed} unhealthy_boots={boots}",
        )

    hist = re.search(r"ring count=(\d+)/(\d+)", responses.get("hist", ""))
    if hist:
        n = int(hist.group(1))
        # The ring is seeded during boot, so even a fresh boot has one sample.
        record(PASS if n >= 1 else FAIL, "history ring collecting", f"{n} samples")
    else:
        record(FAIL, "history ring collecting", "no hist response")

    batt = responses.get("batt", "")
    if re.search(r"\d\.\d+V \d+%", batt):
        record(PASS, "fuel gauge", batt.strip().splitlines()[-1])
    else:
        # nan/-1 with the pack disconnected is expected; the retry probes live.
        record(WARN, "fuel gauge", "no reading (pack disconnected, or gauge absent)")

    sto = re.search(r"mounted=(\d) type=(\S+)", responses.get("storage", ""))
    if sto and sto.group(1) == "1":
        record(PASS, "storage mounted", sto.group(2))
    else:
        record(WARN, "storage mounted", "not mounted (no/wedged card and no FFat)")
    return ip


def mqtt_roundtrip(env: dict) -> None:
    host = env.get("MQTT_HOST", "")
    if not host:
        record(WARN, "mqtt round-trip", "no MQTT_HOST in .env; skipped")
        return
    port = env.get("MQTT_PORT", "1883")
    auth = ["-u", env.get("MQTT_USER", ""), "-P", env.get("MQTT_PASSWORD", "")]

    try:
        # Collect every retained availability topic for a few seconds rather
        # than taking the first: brokers can carry ghost topics from old client
        # ids (espsensor/unknown/... from the pre-fix efuse bug) and the first
        # retained delivery is whichever the broker feels like sending.
        avail = subprocess.run(
            [
                "mosquitto_sub",
                "-h",
                host,
                "-p",
                port,
                *auth,
                "-t",
                "espsensor/+/availability",
                "-v",
                "-W",
                "5",
            ],
            capture_output=True,
            text=True,
            timeout=12,
        ).stdout.strip()
    except (subprocess.TimeoutExpired, FileNotFoundError) as e:
        record(WARN, "mqtt round-trip", f"mosquitto clients unavailable ({e.__class__.__name__})")
        return
    online = {}
    for line in avail.splitlines():
        m = re.match(r"espsensor/(\S+)/availability (\S+)", line)
        if m and m.group(1) != "unknown":
            online[m.group(1)] = m.group(2)
    live = [d for d, s in online.items() if s == "online"]
    if not live:
        record(FAIL, "device availability on broker", f"no online device in: {online or avail!r}")
        return
    device_id = live[0]
    record(PASS, "device availability on broker", f"{device_id}: online")

    # Command round-trip: heap over cmd/debug, JSON back on debug/response.
    sub = subprocess.Popen(
        [
            "mosquitto_sub",
            "-h",
            host,
            "-p",
            port,
            *auth,
            "-t",
            f"espsensor/{device_id}/debug/response",
            "-C",
            "1",
            "-W",
            "20",
        ],
        stdout=subprocess.PIPE,
        text=True,
    )
    time.sleep(1)
    subprocess.run(
        [
            "mosquitto_pub",
            "-h",
            host,
            "-p",
            port,
            *auth,
            "-t",
            f"espsensor/{device_id}/cmd/debug",
            "-m",
            '{"cmd":"heap"}',
        ],
        timeout=10,
        check=False,
    )
    try:
        resp, _ = sub.communicate(timeout=25)
    except subprocess.TimeoutExpired:
        sub.kill()
        record(FAIL, "mqtt command round-trip", "no debug/response within 20s")
        return
    try:
        data = json.loads(resp.strip())
        record(PASS, "mqtt command round-trip", f"heap free={data.get('free')}")
        record(
            PASS if isinstance(data.get("free"), int) and data["free"] > 8192 else WARN,
            "free heap sane",
            f"{data.get('free')} bytes",
        )
    except (json.JSONDecodeError, AttributeError):
        record(FAIL, "mqtt response is valid JSON", resp.strip()[:120])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", help="serial port (default: first /dev/cu.usbmodem*)")
    ap.add_argument("--skip-mqtt", action="store_true", help="serial checks only")
    ap.add_argument("--skip-serial", action="store_true", help="MQTT checks only")
    args = ap.parse_args()

    print("=== room node smoke test ===")
    if not args.skip_serial:
        port = args.port or autodetect_port()
        if not port:
            record(FAIL, "serial port", "no /dev/cu.usbmodem* found (is the device on USB?)")
        else:
            print(f"serial: {port}")
            try:
                check_serial(serial_console(port))
            except Exception as e:  # port vanished mid-run, permissions, ...
                record(FAIL, "serial console", f"{e.__class__.__name__}: {e}")

    if not args.skip_mqtt:
        mqtt_roundtrip(load_dotenv())

    fails = [r for r in RESULTS if r[0] == FAIL]
    warns = [r for r in RESULTS if r[0] == WARN]
    print(
        f"\n{len(RESULTS)} checks: {len(RESULTS) - len(fails) - len(warns)} pass, "
        f"{len(warns)} warn, {len(fails)} fail"
    )
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
