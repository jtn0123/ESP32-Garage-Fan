#pragma once
// Controller readiness for external monitors. Temperature inputs matter only
// in automatic mode; the barometer and SD card are optional peripherals.

namespace health_status {

struct Snapshot {
  bool mqtt;
  bool confirmed;
  bool automatic;
  bool inside_fresh;
  bool outside_fresh;
  bool power_ok;
  bool actuator_ok;
};

inline bool ready(const Snapshot& s) {
  return s.mqtt && s.confirmed && s.power_ok && s.actuator_ok &&
         (!s.automatic || (s.inside_fresh && s.outside_fresh));
}

}  // namespace health_status
