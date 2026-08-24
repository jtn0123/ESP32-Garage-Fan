// Host tests for what the e-ink panel says. See ui/display_layout.h.
//
// The panel is the one readout you can use with the garage door open and no
// phone in your hand, and it refreshes every five minutes -- so a wrong string
// stays wrong for five minutes and there is no console to cross-check it
// against. Formatting and staleness are worth pinning.
#include <unity.h>

#include <math.h>
#include <string.h>

#include "ui/display_layout.h"

using namespace display_layout;

void setUp() {}
void tearDown() {}

static char buf[64];

// ------------------------------------------------------------------ the fan

static void a_stopped_fan_reads_off_not_zero() {
  speed_text(0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("OFF", buf);
  // And it carries no "/12": a stopped fan is not step zero of twelve.
  speed_scale_text(0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("", buf);
}

static void a_running_fan_shows_its_step_and_scale() {
  speed_text(7, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("7", buf);
  speed_scale_text(7, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("/12", buf);
}

// A manual duty from /api/raw shows as a negative speed. It must not render as
// a bare minus sign or blow the field width.
static void a_negative_speed_reads_as_off() {
  speed_text(-4, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("OFF", buf);
}

static void the_bar_spans_zero_to_full() {
  TEST_ASSERT_EQUAL_FLOAT(0.0f, speed_fraction(0));
  TEST_ASSERT_EQUAL_FLOAT(1.0f, speed_fraction(12));
  TEST_ASSERT_EQUAL_FLOAT(0.5f, speed_fraction(6));
  // Clamped: a speed past the table must not draw past the box.
  TEST_ASSERT_EQUAL_FLOAT(1.0f, speed_fraction(99));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, speed_fraction(-1));
}

// ---------------------------------------------------------- temperatures

static void temperatures_render_in_fahrenheit() {
  temp_f_text(25.0f, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("77.0", buf);
  temp_f_text(0.0f, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("32.0", buf);
}

// A missing probe must be NAMED, not blank and not zero: 0.0 is a plausible
// reading and a blank box looks like a rendering bug.
static void a_missing_reading_is_named_not_blank() {
  temp_f_text(NAN, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("--.-", buf);
  rh_text(NAN, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("RH --", buf);
  battery_text(NAN, NAN, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("BAT --", buf);
}

static void humidity_renders_whole_percent() {
  rh_text(41.4f, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("RH 41%", buf);
}

// ---------------------------------------------------------- differential

static void the_differential_is_inside_minus_outside_in_f() {
  // 25 C garage, 20 C yard -> 5 C -> 9.0 F
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 9.0f, differential_f(25.0f, 20.0f));
  differential_text(25.0f, 20.0f, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("DIFF +9.0", buf);
}

// A colder garage than the yard is a real state (night, door open) and the sign
// is the whole point -- an unsigned number would read as "9 degrees hotter".
static void a_cold_garage_shows_a_negative_differential() {
  differential_text(18.0f, 22.0f, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("DIFF -7.2", buf);
}

// THE ONE THAT MATTERS: with one side missing the difference is not zero, it is
// unknown. Rendering 0.0 would read as "equalised", which is the exact state
// that stops the fan -- so a dead outdoor feed would look like a reason to be
// off rather than like a missing reading.
static void a_missing_side_makes_the_differential_unknown_not_zero() {
  TEST_ASSERT_TRUE(isnan(differential_f(25.0f, NAN)));
  TEST_ASSERT_TRUE(isnan(differential_f(NAN, 20.0f)));
  differential_text(25.0f, NAN, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("DIFF --", buf);
  TEST_ASSERT_FALSE(differential_is_hot(25.0f, NAN, 2.5f));
}

static void the_hot_flag_follows_the_engage_threshold() {
  TEST_ASSERT_TRUE(differential_is_hot(25.0f, 20.0f, 2.5f));   // 9.0 >= 2.5
  TEST_ASSERT_FALSE(differential_is_hot(21.0f, 20.0f, 2.5f));  // 1.8 < 2.5
  // Exactly at the threshold counts as engaged, matching the firmware's >=.
  TEST_ASSERT_TRUE(differential_is_hot(20.0f + 2.5f * 5.0f / 9.0f, 20.0f, 2.5f));
}

// ------------------------------------------------------------ footer bits

static void mode_names_who_is_driving() {
  mode_text(true, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("AUTO", buf);
  mode_text(false, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("MANUAL", buf);
}

// Two-digit minutes keep the footer a fixed width. On e-ink a string that
// changes width redraws a region every refresh for no new information.
static void runtime_keeps_a_stable_width() {
  runtime_text(0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("0h00m", buf);
  runtime_text(3600 + 5 * 60, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("1h05m", buf);
  runtime_text(16501, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("4h35m", buf);
}

static void battery_drops_the_percent_when_the_gauge_has_none() {
  battery_text(4.19f, 100.0f, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("4.19V 100%", buf);
  battery_text(4.19f, NAN, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("4.19V", buf);
}

// --------------------------------------------------------------- cadence

static void the_first_boot_paints_immediately() {
  // never_rendered wins over every other rule: a fresh boot must not sit blank
  // for a whole sample period with the fan already running.
  TEST_ASSERT_TRUE(refresh_due(0, 0, 300000, 0, 0, 60000, true));
}

static void the_sample_cadence_drives_the_refresh() {
  TEST_ASSERT_FALSE(refresh_due(299000, 0, 300000, 5, 5, 60000, false));
  TEST_ASSERT_TRUE(refresh_due(300000, 0, 300000, 5, 5, 60000, false));
}

// A hand walking the rail from 1 to 12 must not strobe the panel twelve times.
// Each refresh parks the loop for seconds and cost this device its MQTT
// session once already.
static void a_speed_change_waits_out_the_settle_period() {
  TEST_ASSERT_FALSE(refresh_due(30000, 0, 300000, 9, 3, 60000, false));
  TEST_ASSERT_TRUE(refresh_due(61000, 0, 300000, 9, 3, 60000, false));
  // Same speed, inside the sample window: nothing to say, so no refresh.
  TEST_ASSERT_FALSE(refresh_due(61000, 0, 300000, 3, 3, 60000, false));
}

// millis() wraps after ~49 days. Unsigned subtraction stays correct across the
// wrap; a signed comparison here would freeze the panel for weeks.
static void the_cadence_survives_the_millis_wrap() {
  const uint32_t last = 0xFFFFF000;
  const uint32_t now = last + 300000;  // wrapped
  TEST_ASSERT_TRUE(refresh_due(now, last, 300000, 5, 5, 60000, false));
  TEST_ASSERT_FALSE(refresh_due(last + 1000, last, 300000, 5, 5, 60000, false));
}

// ------------------------------------------------------------- overflow

// Every string lands in a fixed box on a 250 px panel. A caller passing a small
// buffer must get a truncated string, never a smashed stack.
static void every_formatter_respects_a_tiny_buffer() {
  char small[4];
  temp_f_text(123.4f, small, sizeof(small));
  TEST_ASSERT_TRUE(strlen(small) < sizeof(small));
  rh_text(100.0f, small, sizeof(small));
  TEST_ASSERT_TRUE(strlen(small) < sizeof(small));
  differential_text(25.0f, 20.0f, small, sizeof(small));
  TEST_ASSERT_TRUE(strlen(small) < sizeof(small));
  runtime_text(999999, small, sizeof(small));
  TEST_ASSERT_TRUE(strlen(small) < sizeof(small));
  battery_text(4.19f, 100.0f, small, sizeof(small));
  TEST_ASSERT_TRUE(strlen(small) < sizeof(small));
  speed_text(12, small, sizeof(small));
  TEST_ASSERT_TRUE(strlen(small) < sizeof(small));
}

static void a_null_or_zero_buffer_is_survivable() {
  temp_f_text(25.0f, nullptr, 0);
  rh_text(50.0f, buf, 0);
  differential_text(25.0f, 20.0f, nullptr, 10);
  runtime_text(60, nullptr, 0);
  TEST_ASSERT_TRUE(true);  // reaching here without a fault is the assertion
}

static void the_fault_banner_outranks_the_mode() {
  char b[16];
  display_layout::banner_status_text(true, 1, false, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("AUTO", b);
  display_layout::banner_status_text(false, 0, false, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("MANUAL", b);
  // The watt meter calling the fan a liar outranks whoever is driving.
  display_layout::banner_status_text(true, -1, false, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("FAN FAULT", b);
  display_layout::banner_status_text(false, -1, false, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("FAN FAULT", b);
  // And the cycling profile outranks the plain fault: it is the more
  // specific finding, and the verdict flaps underneath it while a fan cycles.
  display_layout::banner_status_text(true, -1, true, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("FAN CYCLING", b);
  display_layout::banner_status_text(true, 1, true, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("FAN CYCLING", b);
}

// Formatter only: the freshness gate lives in display.cpp's compose(),
// which needs hardware -- here we pin what each input renders as.
static void power_formats_valid_readings() {
  char b[12];
  display_layout::power_text(20.3f, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("20W", b);
  display_layout::power_text(NAN, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("", b);
  display_layout::power_text(-1.0f, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("", b);
}

static void voc_names_the_warmup_instead_of_blanking() {
  char b[16];
  display_layout::voc_text(156, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("VOC 156", b);
  display_layout::voc_text(0, b, sizeof(b));  // warming: a blank would read as no sensor
  TEST_ASSERT_EQUAL_STRING("VOC WARM", b);
  display_layout::voc_text(-1, b, sizeof(b));  // truly no sensor: silence
  TEST_ASSERT_EQUAL_STRING("", b);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(a_stopped_fan_reads_off_not_zero);
  RUN_TEST(a_running_fan_shows_its_step_and_scale);
  RUN_TEST(a_negative_speed_reads_as_off);
  RUN_TEST(the_bar_spans_zero_to_full);
  RUN_TEST(temperatures_render_in_fahrenheit);
  RUN_TEST(a_missing_reading_is_named_not_blank);
  RUN_TEST(humidity_renders_whole_percent);
  RUN_TEST(the_differential_is_inside_minus_outside_in_f);
  RUN_TEST(a_cold_garage_shows_a_negative_differential);
  RUN_TEST(a_missing_side_makes_the_differential_unknown_not_zero);
  RUN_TEST(the_hot_flag_follows_the_engage_threshold);
  RUN_TEST(mode_names_who_is_driving);
  RUN_TEST(the_fault_banner_outranks_the_mode);
  RUN_TEST(power_formats_valid_readings);
  RUN_TEST(voc_names_the_warmup_instead_of_blanking);
  RUN_TEST(runtime_keeps_a_stable_width);
  RUN_TEST(battery_drops_the_percent_when_the_gauge_has_none);
  RUN_TEST(the_first_boot_paints_immediately);
  RUN_TEST(the_sample_cadence_drives_the_refresh);
  RUN_TEST(a_speed_change_waits_out_the_settle_period);
  RUN_TEST(the_cadence_survives_the_millis_wrap);
  RUN_TEST(every_formatter_respects_a_tiny_buffer);
  RUN_TEST(a_null_or_zero_buffer_is_survivable);
  return UNITY_END();
}
