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
