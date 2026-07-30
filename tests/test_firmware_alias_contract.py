import os

ROOT = os.path.dirname(os.path.dirname(__file__))


def _read(path: str) -> str:
    with open(path, "r") as f:
        return f.read()


def test_alias_subscriptions_and_callbacks_present():
    # Check that MQTT client subscribes to outdoor alias topics and handles them
    mqtt_client = os.path.join(ROOT, "firmware", "arduino", "src", "mqtt_client.cpp")
    txt = _read(mqtt_client)

    # Subscription checks - look for the subscription to alias topics
    assert '"/temp_f"' in txt, "Should subscribe to temp_f alias"
    assert '"/pressure_hpa"' in txt, "Should subscribe to pressure_hpa alias"
    assert '"/condition"' in txt, "Should subscribe to condition alias"
    assert '"/condition_code"' in txt, "Should subscribe to condition_code alias"

    # Callback checks - verify the callback handles these topics.
    # The suffix match is done by a topic_ends_with lambda over `topic`; it used
    # to be ends_with(topicStr, ...), and these assertions still named the old
    # form long after the rename, so they failed while the handling was present.
    assert 'topic_ends_with(topic, "/temp_f")' in txt, "Should handle temp_f in callback"
    assert 'topic_ends_with(topic, "/condition")' in txt, "Should handle condition in callback"
    assert (
        'topic_ends_with(topic, "/condition_code")' in txt
    ), "Should handle condition_code in callback"

    # Verify legacy topic support
    assert '"/temp"' in txt, "Should support legacy temp topic"
    assert '"/pressure"' in txt, "Should support legacy pressure topic"
    assert '"/weather"' in txt, "Should support legacy weather topic"
    assert '"/weather_id"' in txt, "Should support legacy weather_id topic"


def test_outside_pressure_is_ingested_and_stored():
    """The OUT_PRESSURE row stays blank unless the alias payload reaches the struct."""
    mqtt_client = os.path.join(ROOT, "firmware", "arduino", "src", "mqtt_client.cpp")
    txt = _read(mqtt_client)
    assert 'topic_ends_with(topic, "/pressure_hpa")' in txt, "Should handle pressure_hpa alias"
    assert 'topic_ends_with(topic, "/pressure")' in txt, "Should handle legacy pressure alias"
    assert "g_outside.pressureHPa = pressure_hpa;" in txt
    assert "g_outside.validPressure = true;" in txt

    common_types = os.path.join(ROOT, "firmware", "arduino", "src", "common_types.h")
    struct_txt = _read(common_types)
    assert "float pressureHPa" in struct_txt, "OutsideReadings needs a pressure field"
    assert "bool validPressure" in struct_txt, "OutsideReadings needs a pressure validity flag"


def test_unavailable_pressure_payload_clears_validity():
    """The alias topics are retained and Home Assistant publishes
    "unavailable"/"unknown" when the source entity loses its value. An unparseable
    payload must clear validity, or the last good reading stays on screen forever."""
    mqtt_client = os.path.join(ROOT, "firmware", "arduino", "src", "mqtt_client.cpp")
    txt = _read(mqtt_client)
    # The pressure branch's else arm invalidates rather than falling through.
    branch = txt.split('topic_ends_with(topic, "/pressure_hpa")', 1)[1]
    branch = branch.split("} else if", 1)[0]
    assert "g_outside.validPressure = false;" in branch, "invalid payload must clear validity"
    assert "g_outside.pressureHPa = NAN;" in branch, "invalid payload must clear the reading"


def test_debug_state_json_is_valid_before_first_reading():
    """Sensor floats start as NAN and "%.1f" prints "nan", which is not valid JSON:
    a consumer cannot even parse the document to see the value is missing."""
    debug = os.path.join(ROOT, "firmware", "arduino", "src", "debug_commands.cpp")
    txt = _read(debug)
    assert "static void json_number(" in txt, "debug JSON needs a null-emitting number helper"
    assert 'snprintf(out, out_size, "null");' in txt
    # No raw float conversions left in the two payloads that carry sensor readings.
    for cmd in ("cmdState", "cmdSensors"):
        body = txt.split(f"void DebugCommands::{cmd}(", 1)[1].split("\n}\n", 1)[0]
        assert (
            "%.1f" not in body and "%.0f" not in body
        ), f"{cmd} still formats a sensor float directly; route it through json_number()"
