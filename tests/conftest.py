import os
import shutil
import subprocess
import sys
import time

import pytest

ROOT = os.path.dirname(os.path.dirname(__file__))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

from scripts.test_mqtt_integration import MqttTestClient, _now_ms  # noqa: E402


@pytest.fixture(scope="session")
def mosquitto_broker():
    mosq = shutil.which("mosquitto")
    if not mosq:
        pytest.skip("mosquitto not installed; skipping MQTT tests")

    conf = os.path.join(ROOT, "mosquitto_test.conf")
    port = 18884

    # DEVNULL, not PIPE: -v logs every packet, nobody drains the pipe, and a
    # full-suite's worth of traffic fills the 64 KB buffer - at which point
    # mosquitto blocks on its next write and the frozen broker drops clients.
    # That surfaced as rc=4 publish/subscribe failures in whichever test ran
    # after the buffer filled (reproducible only at full-suite volume).
    _blog = open("/tmp/mosq_fixture.log", "w")
    proc = subprocess.Popen(
        [mosq, "-c", conf], stdout=_blog, stderr=subprocess.STDOUT
    )

    host = "127.0.0.1"
    deadline = time.time() + 5.0
    last_err = None
    while time.time() < deadline:
        try:
            probe = MqttTestClient(host, port, client_id=f"probe-{_now_ms()}")
            probe.connect()
            probe.disconnect()
            break
        except Exception as e:  # pragma: no cover - timing dependent
            last_err = e
            time.sleep(0.05)
    else:
        try:
            proc.terminate()
        except Exception:
            pass
        try:
            proc.kill()
        except Exception:
            pass
        pytest.skip(f"Failed to start mosquitto: {last_err}")

    try:
        yield (host, port)
    finally:  # pragma: no cover - teardown
        try:
            proc.terminate()
            proc.wait(timeout=2)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass

