#pragma once
// The MQTT session: availability, retained speed commands, and the health
// signal the OTA rollback logic keys on (an image is only "confirmed" once
// it has reached the broker).
//
// It used to carry the outdoor temperature in as well; that feed was a Home
// Assistant relay racing the firmware's own weather poll for one variable,
// and it was removed in 1.23.0 (sensors/outdoor.h). This link is now
// commands out-and-in only -- nothing the thermostat reads arrives here.
//
// Command flow in: on_message -> fan::apply. Whether a set counts as manual
// (and therefore disables auto) is decided HERE, because only this layer
// knows if it arrived inside the retained-replay grace window right after
// connecting.

namespace mqtt_link {

void init();  // set server + callback (before WiFi is even up)
void tick();  // fixed 3 s reconnect retry + client loop; call every loop()
bool connected();

/** Did this boot ever reach the broker? Feeds the rollback deadline. */
bool ever_connected();

/** Retained speed state, e.g. after a local (non-MQTT) change. */
void publish_state(int speed);

/** Echo a non-MQTT speed change onto the retained command topic. */
void echo_set(int speed);

/** Retained climate sample for anything else on the bus. */
void publish_climate(float t, float h, float p);

/** Retained standing alert on kTopicAlert; see plug::alert_json. */
void publish_alert(const char* payload);

}  // namespace mqtt_link
