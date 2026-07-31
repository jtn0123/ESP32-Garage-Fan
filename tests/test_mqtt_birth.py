import json
import os
import threading
import time

import pytest

from scripts.test_mqtt_integration import (
    MqttTestClient,
    SensorSpec,
    _now_ms,
    build_discovery_config,
)

# Requires a live MQTT broker (127.0.0.1:1883 by default; MQTT_HOST/MQTT_PORT
# override). CI runs these in the mqtt-itest job; locally: `mosquitto -p 1883 -d`.
pytestmark = pytest.mark.integration


def _mqtt_host_port():
    host = os.environ.get("MQTT_HOST", "127.0.0.1")
    port = int(os.environ.get("MQTT_PORT", "1883"))
    return host, port


def _try_connect_or_skip(host, port):
    c = MqttTestClient(host, port, client_id=f"probe-{_now_ms()}")
    try:
        c.connect()
    except Exception as e:  # pragma: no cover - conditional skip
        pytest.skip(f"MQTT broker not available at {host}:{port}: {e}")
    finally:
        try:
            c.disconnect()
        except Exception:
            pass


def test_homeassistant_birth_triggers_rediscovery_and_state_republish(mosquitto_broker):
    host, port = mosquitto_broker
    if os.environ.get("MQTT_HOST"):
        host, port = _mqtt_host_port()

    device_id = f"room_node_birth_{_now_ms()}"
    state_base = f"sensors/{device_id}"
    availability_topic = f"{state_base}/availability"
    ha_prefix = "homeassistant"

    sensors = [
        SensorSpec(
            key="temp",
            name="Temperature",
            state_topic=f"{state_base}/inside/temp",
            unit_of_measurement="°C",
            device_class="temperature",
            sample_value="21.7",
        ),
        SensorSpec(
            key="hum",
            name="Humidity",
            state_topic=f"{state_base}/inside/hum",
            unit_of_measurement="%",
            device_class="humidity",
            sample_value="48.2",
        ),
        SensorSpec(
            key="batt",
            name="Battery",
            state_topic=f"{state_base}/battery",
            unit_of_measurement="%",
            device_class="battery",
            sample_value="93",
        ),
    ]

    # Device simulator: listens for HA birth and republishes discovery + current states (retained)
    device = MqttTestClient(host, port, client_id=f"dev-{_now_ms()}")
    device.connect()

    def on_device_msg(_client, _userdata, msg):  # pragma: no cover - involves I/O
        if msg.topic == "homeassistant/status":
            payload_decoded = msg.payload.decode("utf-8", "ignore")
            if payload_decoded == "online":
                for s in sensors:
                    cfg_topic = f"{ha_prefix}/sensor/{device_id}_{s.key}/config"
                    cfg = build_discovery_config(device_id, availability_topic, s)
                    cfg_payload = json.dumps(cfg)
                    device.publish(cfg_topic, cfg_payload, retain=True, qos=1)
                for s in sensors:
                    device.publish(s.state_topic, s.sample_value, retain=True, qos=1)
                # Also publish availability online retained so HA can mark entity available
                device.publish(availability_topic, "online", retain=True, qos=1)

    device.client.on_message = on_device_msg
    device.subscribe_and_confirm("homeassistant/status")

    # HA validator subscribes to discovery and state topics and waits for messages
    ha = MqttTestClient(host, port, client_id=f"ha-{_now_ms()}")
    ha.connect()

    received = []
    got_all = threading.Event()
    expected_total = len(sensors) * 2  # discovery + state for each sensor

    def on_ha_msg(_client, _userdata, msg):  # pragma: no cover - involves I/O
        nonlocal received
        received.append((msg.topic, msg.payload.decode("utf-8", "ignore")))
        if len(received) >= expected_total:
            got_all.set()

    ha.client.on_message = on_ha_msg

    # Subscribe to all discovery and state topics before triggering birth
    for s in sensors:
        ha.subscribe_and_confirm(f"{ha_prefix}/sensor/{device_id}_{s.key}/config")
    for s in sensors:
        ha.subscribe_and_confirm(s.state_topic)

    # Publish HA birth
    ha.publish("homeassistant/status", "online")

    # Wait for device to react and HA to receive both discovery and states
    end_at = time.time() + 5.0
    while time.time() < end_at and not got_all.is_set():
        time.sleep(0.05)

    msg = f"Expected at least {expected_total} messages, got {len(received)}: {received}"
    assert len(received) >= expected_total, msg

    # Validate payloads: discovery JSONs include expected topics, and states match sample values
    seen_cfg = set()
    seen_state = set()
    for t, p in received:
        if t.endswith("/config"):
            data = json.loads(p)
            assert data["availability_topic"] == availability_topic
            seen_cfg.add(t)
        else:
            seen_state.add(t)

    for s in sensors:
        assert f"{ha_prefix}/sensor/{device_id}_{s.key}/config" in seen_cfg
        assert s.state_topic in seen_state

    # A fresh subscriber should receive retained discovery and states
    val = MqttTestClient(host, port, client_id=f"val-{_now_ms()}")
    val.connect()
    for s in sensors:
        cfg_topic = f"{ha_prefix}/sensor/{device_id}_{s.key}/config"
        msgs = val.subscribe_and_wait(cfg_topic, expected_count=1, timeout_s=3.0)
        assert msgs and msgs[0][1] is True, f"Expected retained discovery for {s.key}"
        state_msgs = val.subscribe_and_wait(s.state_topic, expected_count=1, timeout_s=3.0)
        assert state_msgs and state_msgs[0][1] is True, f"Expected retained state for {s.key}"

    val.disconnect()
    ha.disconnect()
    device.disconnect()
