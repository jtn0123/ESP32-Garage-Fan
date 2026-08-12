// Host tests for climate CSV row decoding. See csv_row.h.
//
// Rows are quoted verbatim from the deployed card, which holds both widths:
// six fields before 1.14.23, eight after.
#include <unity.h>

#include <cmath>

#include "storage/csv_row.h"

using storage::parse_csv_row;

void setUp() {}
void tearDown() {}

// ------------------------------------------------------------- the two widths

static void reads_a_modern_eight_field_row() {
  const auto r = parse_csv_row("1786466922,26.14,37.8,1001.4,76.40,0,4.20,1");
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_EQUAL_INT32(1786466922, r.epoch);
  TEST_ASSERT_FLOAT_WITHIN(0.001, 26.14, r.temp_c);
  TEST_ASSERT_FLOAT_WITHIN(0.001, 37.8, r.rh);
  TEST_ASSERT_FLOAT_WITHIN(0.001, 1001.4, r.hpa);
  TEST_ASSERT_FLOAT_WITHIN(0.001, 76.40, r.out_f);
  TEST_ASSERT_EQUAL_INT(0, r.spd);
  TEST_ASSERT_FLOAT_WITHIN(0.001, 4.20, r.batt_v);
  TEST_ASSERT_EQUAL_INT(1, r.chg);
}

// The compatibility case: everything logged before 1.14.23. Getting this wrong
// silently drops every older sample from the charts.
static void reads_a_legacy_six_field_row() {
  const auto r = parse_csv_row("1786309531,21.25,32.6,995.9,-999.00,9");
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_EQUAL_INT32(1786309531, r.epoch);
  TEST_ASSERT_FLOAT_WITHIN(0.001, 21.25, r.temp_c);
  TEST_ASSERT_EQUAL_INT(9, r.spd);
  // The columns that did not exist read as absent, NOT as zero -- a battery
  // charted at 0.00 V would look like a dead pack rather than no record.
  TEST_ASSERT_TRUE(isnan(r.batt_v));
  TEST_ASSERT_EQUAL_INT(-1, r.chg);
}

static void a_card_spanning_the_change_reads_whole() {
  const char* rows[] = {
      "1786309531,21.25,32.6,995.9,-999.00,9",        // before
      "1786466622,25.50,37.5,1001.4,76.40,7,4.20,1",  // after
  };
  for (const char* line : rows) TEST_ASSERT_TRUE(parse_csv_row(line).valid);
}

// -------------------------------------------------------------- the sentinels

static void the_outdoor_sentinel_becomes_absent() {
  TEST_ASSERT_TRUE(isnan(parse_csv_row("1786309531,21.2,32.6,995.9,-999.00,9").out_f));
  // And a real cold reading is NOT swallowed by the sentinel test.
  const auto cold = parse_csv_row("1786309531,21.2,32.6,995.9,-40.00,9");
  TEST_ASSERT_FALSE(isnan(cold.out_f));
  TEST_ASSERT_FLOAT_WITHIN(0.01, -40.0, cold.out_f);
}

static void the_battery_sentinel_becomes_absent() {
  const auto r = parse_csv_row("1786466922,26.1,37.8,1001.4,76.4,0,-999.00,-1");
  TEST_ASSERT_TRUE(isnan(r.batt_v));
  TEST_ASSERT_EQUAL_INT(-1, r.chg);
}

// ----------------------------------------------------------------- bad input

static void a_header_line_is_not_a_sample() {
  // /download.csv writes this; a reader that accepted it would chart an epoch
  // of 0 -- January 1970 on the left of every graph.
  TEST_ASSERT_FALSE(parse_csv_row("epoch,temp_c,rh,hpa,outside_f,speed,batt_v,chg").valid);
}

static void torn_and_empty_lines_are_rejected() {
  TEST_ASSERT_FALSE(parse_csv_row("").valid);
  TEST_ASSERT_FALSE(parse_csv_row(nullptr).valid);
  TEST_ASSERT_FALSE(parse_csv_row("1786309531").valid);        // epoch only
  TEST_ASSERT_FALSE(parse_csv_row("1786309531,21.25").valid);  // torn mid-row
  TEST_ASSERT_FALSE(parse_csv_row("1786309531,21.25,32.6").valid);
}

static void a_row_with_the_minimum_fields_is_accepted() {
  const auto r = parse_csv_row("1786309531,21.25,32.6,995.9");
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_TRUE(isnan(r.out_f));
  TEST_ASSERT_EQUAL_INT(0, r.spd);
}

// The value that nearly smashed the stack in write_series: a corrupt row can
// hold anything, and the decoder must hand it back as an ordinary float for
// the writer's isfinite() gate to catch.
static void a_corrupt_magnitude_parses_without_exploding() {
  const auto r = parse_csv_row("1786309531,1e38,32.6,995.9,70.0,9,4.2,1");
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_TRUE(isfinite(r.temp_c));
  TEST_ASSERT_TRUE(r.temp_c > 1e30);
}

static void an_infinite_field_is_reported_as_such_not_silently_zeroed() {
  const auto r = parse_csv_row("1786309531,inf,32.6,995.9,70.0,9,4.2,1");
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_FALSE(isfinite(r.temp_c));  // write_series turns this into null
}

static void negative_speed_from_a_raw_calibration_row_survives() {
  // /api/raw holds speed at -1; the CSV records what happened rather than
  // laundering it.
  const auto r = parse_csv_row("1786309531,21.2,32.6,995.9,70.0,-1,4.2,1");
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_EQUAL_INT(-1, r.spd);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(reads_a_modern_eight_field_row);
  RUN_TEST(reads_a_legacy_six_field_row);
  RUN_TEST(a_card_spanning_the_change_reads_whole);
  RUN_TEST(the_outdoor_sentinel_becomes_absent);
  RUN_TEST(the_battery_sentinel_becomes_absent);
  RUN_TEST(a_header_line_is_not_a_sample);
  RUN_TEST(torn_and_empty_lines_are_rejected);
  RUN_TEST(a_row_with_the_minimum_fields_is_accepted);
  RUN_TEST(a_corrupt_magnitude_parses_without_exploding);
  RUN_TEST(an_infinite_field_is_reported_as_such_not_silently_zeroed);
  RUN_TEST(negative_speed_from_a_raw_calibration_row_survives);
  return UNITY_END();
}
