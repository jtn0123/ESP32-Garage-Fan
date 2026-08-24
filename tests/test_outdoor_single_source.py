"""The outdoor temperature has exactly one writer.

Until 1.23.0 two feeds raced for it -- the firmware's own open-meteo poll and
a Home Assistant relay on `home/outdoor/temp_f`, last write wins. Both were
weather SERVICES, they disagreed about the afternoon by up to 4.5 degF, and
the differential the thermostat compares therefore alternated across the whole
1 degF hysteresis band every five minutes on nothing but which feed wrote last
(measured 2026-08-21: nine commanded latch reversals in fifty-five minutes
while the garage reading moved 0.1 degF).

Deleting the relay is not self-defending: nothing in the build fails if a
future edit re-subscribes to an outdoor topic, or adds a second caller of
climate::set_outside_f, and the symptom is a slow oscillation nobody notices
for a week. That is what this file is for.
"""

from __future__ import annotations

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "firmware" / "arduino" / "src"


def sources() -> list[Path]:
    files = [p for p in SRC.rglob("*") if p.suffix in {".cpp", ".h"} and "generated_" not in p.name]
    assert files, f"no C++ sources under {SRC}"
    return files


def strip_comments(text: str) -> str:
    """Drop // and /* */ comments -- they discuss the old feed on purpose."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def test_only_weather_writes_the_outdoor_temperature() -> None:
    # The qualified spelling: climate.cpp holds the definition, everyone else
    # who writes the value has to say which namespace they are reaching into.
    callers = {
        p.relative_to(SRC).as_posix()
        for p in sources()
        if p.suffix == ".cpp" and "climate::set_outside_f(" in strip_comments(p.read_text())
    }
    assert callers == {"net/weather.cpp"}, (
        "climate::set_outside_f must have exactly one caller -- the firmware's own "
        f"open-meteo poll. Found: {sorted(callers)}. A second writer re-creates the "
        "1.22-and-earlier race; if a fallback source is genuinely wanted, it needs a "
        "primary/backup selector with a debounced handover, not last-write-wins."
    )


def test_mqtt_subscribes_to_commands_only() -> None:
    link = strip_comments((SRC / "net" / "mqtt_link.cpp").read_text())
    topics = re.findall(r"\.subscribe\(\s*([^)]+?)\s*\)", link)
    assert topics == ["kTopicSet"], (
        "the broker link carries fan commands only since 1.23.0 -- nothing the "
        f"thermostat reads may arrive over MQTT. Found subscriptions: {topics}"
    )


def test_no_outdoor_topic_is_configured_anywhere() -> None:
    """The topic constant and its MQTT_SUB_BASE root are both gone."""
    offenders = [
        p.relative_to(ROOT).as_posix()
        for p in sources()
        if re.search(r"\b(kTopicOutdoor|MQTT_SUB_BASE)\b", strip_comments(p.read_text()))
    ]
    assert not offenders, f"outdoor topic plumbing came back in: {offenders}"

    gen = (ROOT / "scripts" / "gen_device_header.py").read_text()
    assert "MQTT_SUB_BASE" not in gen, (
        "the generator is emitting MQTT_SUB_BASE again; nothing consumes it, and a "
        "dangling subscribe-topic define is how the second feed gets reintroduced."
    )


def test_the_reading_expires() -> None:
    """One source means a dead source must read as NAN, not as stale truth."""
    climate = strip_comments((SRC / "sensors" / "climate.cpp").read_text())
    assert "OutdoorFeed" in climate, (
        "climate.cpp must hold the reading in sensors::OutdoorFeed, which owns both "
        "the poll mean and the expiry; auto reads NAN as 'hold, never guess'."
    )
    config = (SRC / "config.h").read_text()
    assert re.search(r"kOutdoorStaleMs\s*=\s*30\s*\*\s*60\s*\*\s*1000", config), (
        "kOutdoorStaleMs must stay at 30 min -- three missed 10-minute polls. "
        "Shorter blinds the fan on one hiccup; longer lets it act on old weather."
    )
