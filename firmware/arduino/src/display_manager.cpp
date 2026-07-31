// Display management implementation - extracted from main.cpp
#include "display_manager.h"

#if USE_DISPLAY

#include <Arduino.h>
#include <GxEPD2_BW.h>
#include <cstdio>
#include "display_layout.h"
#include "display_layout_aliases.h"
#include "icons.h"
#include "power.h"
#include "net.h"
#include "generated_config.h"
#include "runtime_config.h"

// Check panel configuration
#ifndef EINK_PANEL_DEPG0213BN
#define EINK_PANEL_DEPG0213BN 0
#endif

// Display object - will be moved here from main.cpp later
// For now, we'll access it via extern from main.cpp
#if EINK_PANEL_DEPG0213BN
extern GxEPD2_BW<GxEPD2_213_DEPG0213BN, GxEPD2_213_DEPG0213BN::HEIGHT> display;
#else
extern GxEPD2_BW<GxEPD2_213_GDEY0213B74, GxEPD2_213_GDEY0213B74::HEIGHT> display;
#endif

// Constants needed for display operations
#define HEADER_NAME_Y_ADJ -8

// Note: Layout variables are generated without RECT_ prefix
// Variables are defined in display_layout.h as HEADER_TIME_CENTER, FOOTER_STATUS_, etc.

// Forward declaration for draw_in_region template from main.cpp
// We'll declare it properly to avoid link issues
void draw_in_region_lambda(const int rect[4], void (*fn)(int16_t, int16_t, int16_t, int16_t, void*),
                           void* ctx);

// Placeholder implementations - will be filled with actual code from main.cpp
void display_manager_init() {
  Serial.println("[DISPLAY] Initializing display...");

  // Initialize display hardware
  display.init(115200, true, 2, false);  // Serial speed, initial, pulldown_dis_time, use RST
  display.setRotation(3);                // Landscape orientation
  display.setTextColor(GxEPD_BLACK);
  display.setFullWindow();

  // Clear display with white background
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
  } while (display.nextPage());

#ifdef BOOT_DEBUG
  // Show test pattern during boot
  Serial.println("[DISPLAY] Showing boot test pattern");
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    // Draw test time "12:34"
    display.setTextSize(2);
    display.setCursor(HEADER_TIME_CENTER[0], HEADER_TIME_CENTER[1] + 4);
    display.print("12:34");

    // Draw version in top right if available
    display.setTextSize(1);
    display.setCursor(HEADER_VERSION[0], HEADER_VERSION[1]);
    display.print(F("v"));
    display.print(FW_VERSION);

    // Draw "BOOT TEST" in center
    display.setTextSize(2);
    const char* test_msg = "BOOT TEST";
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(test_msg, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((EINK_WIDTH - w) / 2, (EINK_HEIGHT - h) / 2);
    display.print(test_msg);
  } while (display.nextPage());

  delay(2000);  // Show test pattern for 2 seconds
#endif

  Serial.println("[DISPLAY] Display initialized");
}

// Text width calculation
int16_t text_width_default_font(const char* s, uint8_t size) {
  // Default 5x7 font is 6 px advance per char at size 1
  int16_t count = 0;
  for (const char* p = s; *p; ++p) count++;
  return count * 6 * size;
}

// Make a shortened condition string from weather text
void make_short_condition_cstr(const char* weather, char* out, size_t out_size) {
  if (!out || out_size == 0)
    return;
  out[0] = '\0';
  if (!weather)
    return;
  // Skip leading spaces
  const char* p = weather;
  while (*p == ' ' || *p == '\t') p++;
  // Copy until next separator or end
  size_t i = 0;
  while (*p && i < out_size - 1) {
    char c = *p;
    // Treat common separators and hyphen as delimiters so HA values like
    // "clear-night" or "snowy-rainy" shorten to a single word.
    if (c == ' ' || c == '\t' || c == ',' || c == ';' || c == ':' || c == '/' || c == '-')
      break;
    out[i++] = c;
    p++;
  }
  out[i] = '\0';
}

// Draw static chrome (borders, labels, etc.)
void draw_static_chrome() {
  // Frame and header linework
  display.fillScreen(GxEPD_WHITE);
  // Draw outer border flush to panel extents
  display.drawRect(0, 0, EINK_WIDTH, EINK_HEIGHT, GxEPD_BLACK);
  // Single header underline at Y=18 to match simulator
  display.drawLine(1, 18, EINK_WIDTH - 2, 18, GxEPD_BLACK);
  // Extend the center divider from header to bottom frame
  display.drawLine(125, 18, 125, EINK_HEIGHT - 2, GxEPD_BLACK);
  // Horizontal rule for footer region at Y=84 to match spec
  display.drawLine(1, 84, EINK_WIDTH - 2, 84, GxEPD_BLACK);

  // Header: room name left, time will be drawn separately
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(1);
  display.setCursor(6, 13 + HEADER_NAME_Y_ADJ);
  display.print(rc_room_name());

  // Section labels: left 'INSIDE', right 'OUTSIDE'
  display.setCursor(6, 22);
  display.print(F("INSIDE"));
  display.setCursor(131, 22);
  display.print(F("OUTSIDE"));
  // Version string in HEADER_VERSION region (not HEADER_TIME_CENTER)
  String version_str = String("v") + FW_VERSION;
  display.setCursor(
      HEADER_VERSION[0] + HEADER_VERSION[2] - 2 - text_width_default_font(version_str.c_str(), 1),
      HEADER_VERSION[1] + HEADER_VERSION[3] - 6);
  display.print(F("v"));
  display.print(FW_VERSION);
  // Center time will be drawn later in draw_header_time(_direct)
}

// These functions use draw_in_region template which is complex to move
// They remain in main.cpp for now

void draw_header_time_direct(const char* time_str) {
  int16_t tw = text_width_default_font(time_str, 1);
  int16_t rx = static_cast<int16_t>(HEADER_TIME_CENTER[0] + (HEADER_TIME_CENTER[2] - tw) / 2);
  int16_t by = static_cast<int16_t>(HEADER_TIME_CENTER[1] + HEADER_TIME_CENTER[3] - 6);
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(1);
  display.setCursor(rx, by);
  display.print(time_str);
}

// draw_status_line uses draw_in_region - remains in main.cpp

void draw_status_line_direct(const BatteryStatus& bs, const char* ip_cstr) {
  // 3-row stacked footer layout matching ui_spec.json
  // Row 1: Battery icon + "Batt X.XXV XX%"
  // Row 2: "~XXXd" (days remaining)
  // Row 3: "IP X.X.X.X" (centered in region)

  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(1);

  // Footer content area starts after the divider line at y=84
  // Using fixed coordinates to match ui_spec.json footer_split layout
  const int16_t row1_y = 87;   // Battery row
  const int16_t row2_y = 98;   // Days row
  const int16_t row3_y = 109;  // IP row
  const int16_t left_x = 8;    // Left margin for battery group

  // Row 1: Battery icon + voltage/percent
  const int16_t bw = 13;  // Battery icon width
  const int16_t bh = 7;   // Battery icon height
  const int16_t by = row1_y;
  const int16_t bx = left_x;

  // Draw battery icon outline
  display.drawRect(bx, by, bw, bh, GxEPD_BLACK);
  // Draw battery nub
  display.fillRect(static_cast<int16_t>(bx + bw), static_cast<int16_t>(by + 2), 2, 3, GxEPD_BLACK);

  // Fill battery level (clamp percent to 0-100 to prevent overflow)
  if (bs.percent >= 0 && bw > 2) {
    int pct_clamped = (bs.percent > 100) ? 100 : bs.percent;
    int16_t max_fillw = static_cast<int16_t>(bw - 2);
    int16_t fillw = static_cast<int16_t>(((max_fillw) * (pct_clamped / 100.0f) + 0.5f));
    if (fillw > max_fillw)
      fillw = max_fillw;  // Safety clamp
    if (fillw > 0) {
      display.fillRect(static_cast<int16_t>(bx + 1), static_cast<int16_t>(by + 1), fillw,
                       static_cast<int16_t>(bh - 2), GxEPD_BLACK);
    }
  }

  // Battery text next to icon
  char batt_text[32];
  if (bs.percent >= 0) {
    snprintf(batt_text, sizeof(batt_text), "%.2fV %d%%", bs.voltage, bs.percent);
  } else {
    snprintf(batt_text, sizeof(batt_text), "--V --%% ");
  }
  display.setCursor(left_x + bw + 6, row1_y);
  display.print(batt_text);

  // Row 2: Days remaining
  char days_text[16];
  if (bs.estimatedDays >= 0) {
    snprintf(days_text, sizeof(days_text), "~%dd", bs.estimatedDays);
  } else {
    snprintf(days_text, sizeof(days_text), "~--d");
  }
  display.setCursor(left_x, row2_y);
  display.print(days_text);

  // Row 3: IP address (centered in FOOTER_STATUS region)
  char ip_text[32];
  if (ip_cstr && ip_cstr[0] && strcmp(ip_cstr, "0.0.0.0") != 0) {
    snprintf(ip_text, sizeof(ip_text), "IP %s", ip_cstr);
  } else {
    snprintf(ip_text, sizeof(ip_text), "IP --");
  }
  int16_t tw = text_width_default_font(ip_text, 1);
  int16_t ip_x = FOOTER_STATUS[0] + (FOOTER_STATUS[2] - tw) / 2;
  display.setCursor(ip_x, row3_y);
  display.print(ip_text);
}

#endif  // USE_DISPLAY
