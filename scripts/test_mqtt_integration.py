#!/usr/bin/env python3
"""
MQTT integration check for CI

This script:
- Publishes Home Assistant MQTT Discovery configs for temperature, humidity, and battery
- Publishes retained state messages for each entity
- Flips availability (online/offline/online) and asserts messages are observed
- Re-subscribes with a new client to assert discovery and states are retained

Broker and topics are intentionally simple and unauthenticated for CI usage.
"""

from __future__ import annotations

from dataclasses import dataclass
import json
import os
import threading
import time
from typing import Dict, List, Optional, Tuple

import paho.mqtt.client as mqtt


def _now_ms() -> int:
    return int(time.time() * 1000)


@dataclass
class SensorSpec:
    key: str
    name: str
    state_topic: str
    unit_of_measurement: str
    device_class: str
    sample_value: str


class MqttTestClient:
    def __init__(self, host: str, port: int, client_id: Optional[str] = None) -> None:
        # Guarantee a unique client id. Callers build ids from _now_ms(), and
        # two clients created in the same millisecond collide - the broker then
        # disconnects the older twin, which surfaced as rc=4 (not connected)
        # publish/subscribe failures that only reproduced at full-suite speed.
        import uuid as _uuid

        client_id = f"{client_id or 'mqtt-test'}-{_uuid.uuid4().hex[:8]}"
        # Create client compatible with both paho-mqtt 1.x and 2.x.
        # In 2.x, the constructor requires a callback API version; we select VERSION1
        # to keep our existing callback signatures working without changes.
        if hasattr(mqtt, "CallbackAPIVersion"):
            try:
                _cbver = mqtt.CallbackAPIVersion.VERSION1
                # Force MQTT v3.1.1 protocol for broad broker compatibility
                self.client = mqtt.Client(
                    client_id=client_id,
                    callback_api_version=_cbver,
                    protocol=mqtt.MQTTv311,
                )
            except TypeError:
                # Older 1.x that doesn't accept the keyword
                self.client = mqtt.Client(client_id=client_id, protocol=mqtt.MQTTv311)
        else:
            # paho-mqtt 1.x
            self.client = mqtt.Client(client_id=client_id, protocol=mqtt.MQTTv311)
        self._host = host
        self._port = port
        # Optional credentials (for secured brokers)
        self._user = os.environ.get("MQTT_USER") or os.environ.get("E2E_MQTT_USER") or ""
        self._pass = os.environ.get("MQTT_PASS") or os.environ.get("E2E_MQTT_PASS") or ""
        self._connected_event = threading.Event()
        self._disconnected_event = threading.Event()
        self._subscribe_cond = threading.Condition()
        self._subscribed_mids: set[int] = set()

        # Support both v1 and v2 callback signatures
        def on_connect(client, userdata, flags, rc=None, properties=None):
            self._connected_event.set()

        def on_disconnect(client, userdata, rc=None, properties=None):
            self._disconnected_event.set()

        self.client.on_connect = on_connect
        self.client.on_disconnect = on_disconnect

        # Handle SUBACK for confirming subscription registration (v1 and v2 compatible)
        def on_subscribe(client, userdata, mid, granted_qos, properties=None):
            with self._subscribe_cond:
                self._subscribed_mids.add(int(mid))
                self._subscribe_cond.notify_all()

        self.client.on_subscribe = on_subscribe

        # Apply credentials if provided
        if self._user:
            try:
                self.client.username_pw_set(self._user, self._pass or None)
            except Exception:
                # Fallback/no-op if library version differs
                pass

    def connect(self, timeout_s: float = 10.0) -> None:
        self.client.connect(self._host, self._port, keepalive=30)
        self.client.loop_start()
        if not self._connected_event.wait(timeout_s):
            raise RuntimeError("MQTT client failed to connect within timeout")

    def disconnect(self, timeout_s: float = 5.0) -> None:
        self.client.disconnect()
        if not self._disconnected_event.wait(timeout_s):
            # Ensure loop stops even if disconnect event didn't arrive
            pass
        self.client.loop_stop()

    def publish(self, topic: str, payload: str, retain: bool = False, qos: int = 0) -> None:
        result = self.client.publish(topic, payload=payload, retain=retain, qos=qos)
        # Do not wait synchronously here. Waiting can deadlock when called from within a
        # callback (network loop thread) and is unnecessary for our tests which already
        # synchronize by subscribing and observing deliveries.
        if result.rc == mqtt.MQTT_ERR_NO_CONN:
            # Suite-order race: the loop thread may briefly report disconnected
            # (e.g. broker restarted by a neighbouring test). Wait for the
            # reconnect the network loop performs automatically, then retry once.
            if self._connected_event.wait(5.0):
                result = self.client.publish(topic, payload=payload, retain=retain, qos=qos)
        if result.rc is not mqtt.MQTT_ERR_SUCCESS:
            raise RuntimeError(f"Publish failed rc={result.rc} topic={topic}")

    def subscribe_and_confirm(self, topic: str, qos: int = 0, timeout_s: float = 3.0) -> None:
        # Subscribe and wait for SUBACK to ensure broker registered the subscription
        result, mid = self.client.subscribe(topic, qos=qos)
        if result != mqtt.MQTT_ERR_SUCCESS:
            raise RuntimeError(f"Subscribe failed rc={result} topic={topic}")
        end_at = time.time() + timeout_s
        with self._subscribe_cond:
            while (
                mid is not None and int(mid) not in self._subscribed_mids and time.time() < end_at
            ):
                remaining = end_at - time.time()
                if remaining <= 0:
                    break
                self._subscribe_cond.wait(timeout=remaining)
            # Clean up recorded mid to avoid unbounded growth
            if mid is not None:
                self._subscribed_mids.discard(int(mid))

    def subscribe_and_wait(
        self,
        topic: str,
        expected_count: int = 1,
        timeout_s: float = 5.0,
    ) -> List[Tuple[str, bool]]:
        """
        Subscribe to a topic and wait for messages.
        Returns a list of (payload, retained_flag) tuples in arrival order.
        """
        messages: List[Tuple[str, bool]] = []
        got_all = threading.Event()

        def on_message(client, userdata, msg):
            messages.append((msg.payload.decode("utf-8", "ignore"), bool(msg.retain)))
            if len(messages) >= expected_count:
                got_all.set()

        # Install handler before subscribing to avoid races with retained delivery
        self.client.on_message = on_message
        self.subscribe_and_confirm(topic, qos=0, timeout_s=timeout_s)
        # Give broker a moment to process SUB and send retained
        end_at = time.time() + timeout_s
        while time.time() < end_at:
            if got_all.is_set():
                break
            time.sleep(0.05)
        return messages


def build_discovery_config(
    device_id: str,
    availability_topic: str,
    sensor: SensorSpec,
) -> Dict[str, object]:
    return {
        "name": f"Room Node CI {sensor.name}",
        "unique_id": f"{device_id}_{sensor.key}",
        "state_topic": sensor.state_topic,
        "availability_topic": availability_topic,
        "payload_available": "online",
        "payload_not_available": "offline",
        "device_class": sensor.device_class,
        "unit_of_measurement": sensor.unit_of_measurement,
        "state_class": "measurement",
        "device": {
            "identifiers": [device_id],
            "name": "Room Node CI",
            "manufacturer": "ESP32-Temp-Sensor",
            "model": "CI",
        },
    }


def main() -> None:
    mqtt_host = os.environ.get("MQTT_HOST", "127.0.0.1")
    mqtt_port = int(os.environ.get("MQTT_PORT", "1883"))

    # Topics and specs
    device_id = "room_node_ci"
    state_base = f"sensors/{device_id}"
    availability_topic = f"{state_base}/status"
    debug_topic = f"{state_base}/debug"
    ha_prefix = "homeassistant"

    sensors: List[SensorSpec] = [
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

    publisher = MqttTestClient(mqtt_host, mqtt_port, client_id=f"pub-{_now_ms()}")
    publisher.connect()

    # 1) Publish Home Assistant discovery configs (retained)
    for s in sensors:
        cfg_topic = f"{ha_prefix}/sensor/{device_id}_{s.key}/config"
        cfg_payload = json.dumps(build_discovery_config(device_id, availability_topic, s))
        publisher.publish(cfg_topic, cfg_payload, retain=True, qos=1)

    # 2) Publish retained states for each
    for s in sensors:
        publisher.publish(s.state_topic, s.sample_value, retain=True, qos=1)

    # 3) Flip availability online -> offline -> online; no retain for availability
    publisher.publish(availability_topic, "online", retain=False)
    time.sleep(0.2)
    publisher.publish(availability_topic, "offline", retain=False)
    time.sleep(0.2)
    publisher.publish(availability_topic, "online", retain=False)

    # 3b) Prepare a sample debug JSON payload (non-retained). We'll publish it later
    # after subscribing to the debug topic to ensure the message is received.
    sample_debug = {
        "ms_boot_to_wifi": 1000,
        "ms_wifi_to_mqtt": 500,
        "ms_sensor_read": 120,
        "ms_publish": 40,
        "timeouts": 0,
    }
    import json as _json

    # 4) Validate discovery topics retained (new subscriber should receive retained messages)
    validator = MqttTestClient(mqtt_host, mqtt_port, client_id=f"sub-{_now_ms()}")
    validator.connect()

    for s in sensors:
        cfg_topic = f"{ha_prefix}/sensor/{device_id}_{s.key}/config"
        msgs = validator.subscribe_and_wait(cfg_topic, expected_count=1, timeout_s=3.0)
        assert msgs, f"No discovery message received for {s.key}"
        payload, retained = msgs[0]
        assert retained, f"Discovery message for {s.key} was not retained"
        data = json.loads(payload)
        assert data["state_topic"] == s.state_topic
        assert data["availability_topic"] == availability_topic

    # 5) Validate retained states
    for s in sensors:
        msgs = validator.subscribe_and_wait(s.state_topic, expected_count=1, timeout_s=3.0)
        assert msgs, f"No state message received for {s.key}"
        payload, retained = msgs[0]
        assert retained, f"State for {s.key} was not retained"
        msg = f"Unexpected state for {s.key}: {payload} != {s.sample_value}"
        assert payload == s.sample_value, msg

    # 6) Validate availability toggles (non-retained real-time)
    toggles_sub = MqttTestClient(mqtt_host, mqtt_port, client_id=f"tog-{_now_ms()}")
    toggles_sub.connect()
    # Subscribe before publishing toggles to ensure we capture them
    events: List[Tuple[str, bool]] = []
    got_all = threading.Event()

    def on_msg(client, userdata, msg):
        events.append((msg.payload.decode("utf-8", "ignore"), bool(msg.retain)))
        if len(events) >= 3:
            got_all.set()

    # Install handler before subscribing and confirm SUBACK to avoid missing first event
    toggles_sub.client.on_message = on_msg
    toggles_sub.subscribe_and_confirm(availability_topic, qos=0, timeout_s=3.0)

    # Trigger the three messages
    publisher.publish(availability_topic, "online", retain=False)
    publisher.publish(availability_topic, "offline", retain=False)
    publisher.publish(availability_topic, "online", retain=False)

    end_at = time.time() + 5.0
    while time.time() < end_at and not got_all.is_set():
        time.sleep(0.05)

    msg_count = f"Expected 3 availability events, got {len(events)}: {events}"
    assert len(events) >= 3, msg_count
    observed = [p for p, _r in events[:3]]
    msg_seq = f"Unexpected availability sequence: {observed}"
    assert observed == ["online", "offline", "online"], msg_seq
    # None of these should be retained
    msg_ret = f"Availability events should not be retained: {events[:3]}"
    assert not any(r for _p, r in events[:3]), msg_ret

    # 7) Validate debug topic can be subscribed to and delivers the sample payload.
    # Subscribe first, then publish a sample payload to ensure delivery (non-retained).
    dbg_sub = MqttTestClient(mqtt_host, mqtt_port, client_id=f"dbg-{_now_ms()}")
    dbg_sub.connect()
    dbg_events: List[Tuple[str, bool]] = []
    got_dbg = threading.Event()

    def on_dbg(client, userdata, msg):
        dbg_events.append((msg.payload.decode("utf-8", "ignore"), bool(msg.retain)))
        got_dbg.set()

    dbg_sub.client.on_message = on_dbg
    dbg_sub.subscribe_and_confirm(debug_topic, qos=0, timeout_s=3.0)

    # Publish after subscription to avoid missing the non-retained message
    publisher.publish(debug_topic, _json.dumps(sample_debug), retain=False)

    end_at = time.time() + 3.0
    while time.time() < end_at and not got_dbg.is_set():
        time.sleep(0.05)

    assert dbg_events, "No debug JSON message received"
    dbg_payload, dbg_retained = dbg_events[0]
    assert not dbg_retained, "Debug JSON should not be retained"
    d = _json.loads(dbg_payload)
    assert d.get("timeouts") == 0
    dbg_sub.disconnect()

    toggles_sub.disconnect()
    validator.disconnect()
    publisher.disconnect()

    print("MQTT integration test passed: discovery retained, states retained, availability toggled")


if __name__ == "__main__":
    main()
