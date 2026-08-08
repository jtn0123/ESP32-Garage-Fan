// Fan-controller rehearsal: replay the wall controller's PWM table on A0
// through the BSS138 shifter so the logic analyzer can diff this board
// against the recordings of the real controller.
//
// Protocol (docs/fan_protocol/PROTOCOL.md): 9934 us period (~100.7 Hz),
// active-low — fan power is the LOW fraction; setting 0 = line held HIGH.
// RMT with a 1 MHz tick replays the measured widths exactly; LEDC would
// round the period to an integer Hz.
#include <Arduino.h>

#ifndef FAN_PWM_PIN
#define FAN_PWM_PIN 18  // A0 -> shifter LV1 -> HV1 (fan D+ side)
#endif
#ifndef FAN_SPARE_PIN
#define FAN_SPARE_PIN 17  // A1 -> shifter LV2; idles HIGH like the real D-
#endif

struct Step {
  uint8_t setting;
  uint16_t high_us;
};

static constexpr uint16_t kPeriodUs = 9934;
// Measured HIGH width per setting, verbatim from PROTOCOL.md.
static constexpr Step kTable[] = {
    {0, kPeriodUs}, {1, 6457}, {2, 5862}, {3, 5066},  {4, 4868}, {5, 4273}, {6, 3775},
    {7, 3180},      {8, 2683}, {9, 2087}, {10, 1590}, {11, 994}, {12, 497},
};
static constexpr uint32_t kDwellMs = 4000;

static bool g_rmt_ready = false;
static rmt_data_t g_wave;

static void drive_setting(const Step& s) {
  // Pin stays owned by RMT; "off" is a wave that never goes low. Handing the
  // pin back to plain GPIO left it stuck at the RMT's last level (LOW = full
  // throttle on this active-low link).
  if (!g_rmt_ready) {
    if (!rmtInit(FAN_PWM_PIN, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 1000000)) {
      Serial.println("rmtInit FAILED");
      return;
    }
    g_rmt_ready = true;
  }
  if (s.high_us >= kPeriodUs) {
    g_wave.level0 = 1;
    g_wave.duration0 = kPeriodUs / 2;
    g_wave.level1 = 1;
    g_wave.duration1 = kPeriodUs - kPeriodUs / 2;
  } else {
    g_wave.level0 = 1;
    g_wave.duration0 = s.high_us;
    g_wave.level1 = 0;
    g_wave.duration1 = kPeriodUs - s.high_us;
  }
  rmtWriteLooping(FAN_PWM_PIN, &g_wave, 1);
}

void setup() {
  pinMode(FAN_PWM_PIN, OUTPUT);
  digitalWrite(FAN_PWM_PIN, HIGH);  // boot in the "off" state
  pinMode(FAN_SPARE_PIN, OUTPUT);
  digitalWrite(FAN_SPARE_PIN, HIGH);
  Serial.begin(115200);
  Serial.println("fan rehearsal: cycling 13 settings, 4 s each");
}

void loop() {
  for (const Step& s : kTable) {
    Serial.printf("setting %u: high %u us / low %u us\n", s.setting, s.high_us,
                  kPeriodUs - s.high_us);
    drive_setting(s);
    delay(kDwellMs);
  }
}
