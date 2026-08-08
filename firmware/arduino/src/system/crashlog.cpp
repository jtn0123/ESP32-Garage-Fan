// See crashlog.h. Definitions only -- the RTC attributes must land in exactly
// one translation unit.
#include "system/crashlog.h"

namespace crashlog {

RTC_DATA_ATTR uint32_t rtc_sd_sentinel = 0;
RTC_DATA_ATTR char rtc_crumb[16] = {0};

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
