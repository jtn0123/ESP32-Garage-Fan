// See crashlog.h. Definitions only -- the RTC attributes must land in exactly
// one translation unit.
#include "system/crashlog.h"

namespace crashlog {

RTC_DATA_ATTR uint32_t rtc_sd_sentinel = 0;
RTC_DATA_ATTR char rtc_crumb[16] = {0};

namespace {
char g_last_death[48] = "none";
char g_prev_death[48] = "none";  // the boot before this one, from NVS
uint32_t g_boots = 0;            // lifetime boot count, NVS
}  // namespace

bool examine_boot(Preferences* prefs) {
  const esp_reset_reason_t rr = esp_reset_reason();
  const bool abnormal = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT ||
                        rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT;
  snprintf(g_last_death, sizeof(g_last_death), "%s%s%s", reset_reason_str(rr),
           rtc_crumb[0] ? " during " : "", rtc_crumb[0] ? rtc_crumb : "");
  // SD suspect on a mount sentinel from any reset, or any abnormal death
  // while an SD op was in flight.
  const bool sd_suspect =
      (rtc_sd_sentinel == kSdSentinelMagic) || (abnormal && strncmp(rtc_crumb, "sd", 2) == 0);
  rtc_sd_sentinel = 0;
  rtc_crumb[0] = 0;
  Serial.printf("last reset: %s\n", g_last_death);
  // Longitudinal forensics: RTC evidence dies with power, NVS doesn't. Keep
  // the prior boot's certificate and a lifetime boot counter so a reboot
  // storm is visible from /api/state even after the fact.
  if (prefs) {
    String pd = prefs->getString("pdeath", "none");
    snprintf(g_prev_death, sizeof(g_prev_death), "%s", pd.c_str());
    prefs->putString("pdeath", g_last_death);
    g_boots = prefs->getUInt("boots", 0) + 1;
    prefs->putUInt("boots", g_boots);
  }
  return sd_suspect;
}

const char* last_death() { return g_last_death; }
const char* prev_death() { return g_prev_death; }
uint32_t boots() { return g_boots; }

const char* reset_reason_str(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:
      return "poweron";
    case ESP_RST_SW:
      return "sw_reset";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "int_wdt";
    case ESP_RST_TASK_WDT:
      return "task_wdt";
    case ESP_RST_WDT:
      return "wdt";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_DEEPSLEEP:
      return "deepsleep";
    default:
      return "other";
  }
}

}  // namespace crashlog
