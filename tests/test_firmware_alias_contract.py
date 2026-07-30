import os

ROOT = os.path.dirname(os.path.dirname(__file__))

MQTT_CLIENT = os.path.join(ROOT, "firmware", "arduino", "src", "mqtt_client.cpp")
MAIN_CPP = os.path.join(ROOT, "firmware", "arduino", "src", "main.cpp")


def _read(path: str) -> str:
    with open(path, "r") as f:
        return f.read()


def _alias_branch(txt: str, suffix: str) -> str:
    """Return the body of the callback branch that handles `suffix`.

    The branches are one if/else-if chain, so a branch runs from its topic match
    to whichever `else if (topic_ends_with(` starts the next one.
    """
    marker = 'topic_ends_with(topic, "%s")' % suffix
    assert marker in txt, "no callback branch handles %s" % suffix
    return txt.split(marker, 1)[1].split("else if (topic_ends_with(", 1)[0]


def test_alias_subscriptions_and_callbacks_present():
    # Check that MQTT client subscribes to outdoor alias topics and handles them
    mqtt_client = os.path.join(ROOT, "firmware", "arduino", "src", "mqtt_client.cpp")
    txt = _read(mqtt_client)

    # Subscription checks - look for the subscription to alias topics
    assert '"/temp_f"' in txt, "Should subscribe to temp_f alias"
    assert '"/rh"' in txt, "Should subscribe to rh alias"
    assert '"/wind_mps"' in txt, "Should subscribe to wind_mps alias"
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
    assert '"/hum"' in txt, "Should support legacy hum topic"
    assert '"/wind"' in txt, "Should support legacy wind topic"
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


def test_unavailable_temperature_payload_clears_validity():
    """Same retained-topic hazard as pressure. Temperature only assigned on a good
    parse, so "unavailable" left the previous reading in place with validTemp still
    set and the display kept rendering a stale number as if it were current."""
    txt = _read(MQTT_CLIENT)
    for suffix in ("/temp_f", "/temp"):
        branch = _alias_branch(txt, suffix)
        assert "g_outside.validTemp = false;" in branch, (
            "%s: invalid payload must clear validity" % suffix
        )
        assert "g_outside.temperatureC = NAN;" in branch, (
            "%s: invalid payload must clear the reading" % suffix
        )


def test_unavailable_condition_payload_clears_weather_validity():
    """A failed strtof is what catches the sentinel for numeric topics; the text
    topics have no such signal. Stored verbatim, "unavailable" became the weather
    condition and fell through iconMap's default arm as a confident sunny icon."""
    txt = _read(MQTT_CLIENT)
    assert "static bool is_no_value_payload(" in txt, "text aliases need a sentinel check"
    sentinel = txt.split("static bool is_no_value_payload(", 1)[1].split("\n}\n", 1)[0]
    assert 'strcasecmp(s, "unavailable")' in sentinel
    assert 'strcasecmp(s, "unknown")' in sentinel
    assert "s[0] == '\\0'" in sentinel, "an empty (cleared) retained payload is also no value"

    for suffix in ("/condition", "/weather"):
        branch = _alias_branch(txt, suffix)
        assert "is_no_value_payload(value_str)" in branch, "%s: should reject sentinels" % suffix
        assert "g_outside.validWeather = false;" in branch, (
            "%s: sentinel payload must clear validity" % suffix
        )
        assert "g_outside.weather[0] = '\\0';" in branch, (
            "%s: sentinel payload must clear the stored condition" % suffix
        )


def test_sim_and_firmware_agree_on_no_value_sentinels():
    """web/sim/sim.js already skips these sentinels via its UNAVAILABLE list. The
    firmware has to reject the same set or the two renderers disagree about
    whether a row has a value."""
    sim_js = _read(os.path.join(ROOT, "web", "sim", "sim.js"))
    sentinel = _read(MQTT_CLIENT).split("static bool is_no_value_payload(", 1)[1]
    sentinel = sentinel.split("\n}\n", 1)[0]
    for word in ("unavailable", "unknown", "none"):
        assert "'%s'" % word in sim_js or '"%s"' % word in sim_js, "sim.js should skip %s" % word
        assert '"%s"' % word in sentinel, "firmware should skip %s" % word


def test_outside_humidity_and_wind_are_ingested():
    """The OUT_HUMIDITY and OUT_WIND rows were dead: nothing wrote humidityPct or
    windMps, so validHum/validWind were never set, even though the topics are
    published by homeassistant/mqtt_outdoor_publish.yaml and seeded by
    scripts/mqtt_headless_check.py."""
    txt = _read(MQTT_CLIENT)

    hum = _alias_branch(txt, "/hum")
    assert "g_outside.humidityPct = hum_pct;" in hum
    assert "g_outside.validHum = true;" in hum

    wind = _alias_branch(txt, "/wind_mps")
    assert "g_outside.windMps = wind_mps;" in wind
    assert "g_outside.validWind = true;" in wind

    # The alternate spellings share a branch with the canonical ones.
    assert 'topic_ends_with(topic, "/rh")' in txt, "Should handle the /rh humidity alias"
    assert 'topic_ends_with(topic, "/wind")' in txt, "Should handle the /wind alias"


def test_unavailable_humidity_and_wind_payloads_clear_validity():
    """Same retained-topic hazard as temperature, for the two rows just wired up."""
    txt = _read(MQTT_CLIENT)

    hum = _alias_branch(txt, "/hum")
    assert "g_outside.validHum = false;" in hum
    assert "g_outside.humidityPct = NAN;" in hum

    wind = _alias_branch(txt, "/wind_mps")
    assert "g_outside.validWind = false;" in wind
    assert "g_outside.windMps = NAN;" in wind


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
