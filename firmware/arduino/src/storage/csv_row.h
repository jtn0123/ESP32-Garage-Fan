#pragma once
// Parsing one row of a climate CSV, separated so the host can test it.
//
// This is the data-integrity path behind every chart and the whole /download.csv
// export, and it has to read two formats at once: rows written before 1.14.23
// carry six fields, rows written after carry eight (batt_v and chg were
// appended). A card that spans the change contains both, and getting the
// fallback wrong silently drops every older row from the charts.
//
// -999 is the "no reading" sentinel for both float columns. It reaches the
// wire as null, never as a temperature of minus a thousand.

#include <stdlib.h>
#include <string.h>

#include <cmath>
#include <cstdio>

namespace storage {

/** One decoded sample. NAN / -1 mean "the row did not record this". */
struct CsvRow {
  long epoch = 0;
  float temp_c = NAN;
  float rh = NAN;
  float hpa = NAN;
  float out_f = NAN;
  float batt_v = NAN;
  float watts = NAN;     // fan draw at the plug (1.14.47+ rows)
  int32_t voc_raw = -1;  // SGP41 raw ticks (1.14.47+ rows)
  int32_t nox_raw = -1;
  int16_t voc = -1;  // gas indices; 0 = algorithm was warming
  int16_t nox = -1;
  float bme_t = NAN;  // the BME280 beside the SHT41 (1.14.48-1.15.1 rows)
  float bme_rh = NAN;
  int8_t flips = -1;  // plug run/stop flips in the bucket (1.21.0+ rows)
  float w_min = NAN;  // the bucket's measured draw range (1.22.0+ rows)
  float w_max = NAN;
  int8_t spd = 0;
  int8_t chg = -1;
  bool valid = false;  // false when the line is not a usable sample
};

/** Anything at or below this is the absent-reading sentinel, not a value. */
constexpr float kAbsentBelow = -100.0f;

/**
 * Decode one CSV line.
 *
 * Accepts every width this firmware has ever written: 6 fields (pre-1.14.23),
 * 8 (batt_v/chg), 13 (watts + the four SGP41 columns, 1.14.47), 15
 * (bme_t/bme_rh, the second thermometer, 1.14.48-1.15.1), 14 (the plug
 * meter's flip count, 1.21.0) and 16 (flips plus the bucket's draw range,
 * 1.22.0). The 14th field is the ONLY ambiguity: in a 15-field row it is
 * bme_t, otherwise it is flips -- the WIDTH decides, and the BME era and the
 * meter era never share a card. A line is valid only with at least epoch +
 * the three always-present readings; anything shorter (a torn tail row, a
 * stray blank, a header) is rejected rather than half-read.
 */
inline CsvRow parse_csv_row(const char* line) {
  CsvRow r;
  if (!line || !*line)
    return r;
  // Reject a non-numeric first field outright, so a header line or a partially
  // written row cannot land as epoch 0 at the start of 1970.
  if (*line < '0' || *line > '9')
    return r;

  // Initialised: sscanf only assigns the fields that exist, and the compiler
  // cannot correlate that with `got`, so -Wmaybe-uninitialized can fire -- and
  // CI builds with -Werror.
  float tv = NAN, hv = NAN, pv = NAN, ov = NAN, bv = NAN, wv = NAN, btv = NAN, bhv = NAN;
  float wxv = NAN;
  int sp = 0, cg = -1, vi = -1, ni = -1;
  long vr = -1, nr = -1;
  const int got = sscanf(line, "%*d,%f,%f,%f,%f,%d,%f,%d,%f,%ld,%ld,%d,%d,%f,%f,%f", &tv, &hv, &pv,
                         &ov, &sp, &bv, &cg, &wv, &vr, &nr, &vi, &ni, &btv, &bhv, &wxv);
  if (got < 3)
    return r;

  r.epoch = strtol(line, nullptr, 10);
  r.temp_c = tv;
  r.rh = hv;
  r.hpa = pv;
  r.out_f = (got >= 4 && ov > kAbsentBelow) ? ov : NAN;
  r.spd = got >= 5 ? static_cast<int8_t>(sp) : 0;
  r.batt_v = (got >= 6 && bv > 0.0f) ? bv : NAN;
  r.chg = got >= 7 ? static_cast<int8_t>(cg) : -1;
  r.watts = (got >= 8 && wv > kAbsentBelow) ? wv : NAN;
  r.voc_raw = got >= 9 ? static_cast<int32_t>(vr) : -1;
  r.nox_raw = got >= 10 ? static_cast<int32_t>(nr) : -1;
  r.voc = got >= 11 ? static_cast<int16_t>(vi) : -1;
  r.nox = got >= 12 ? static_cast<int16_t>(ni) : -1;
  if (got == 13) {
    // 14 fields: the 1.21.0 shape, field 14 is the flip count (-1 = no meter).
    r.flips = (btv >= 0.0f && btv <= 127.0f) ? static_cast<int8_t>(btv) : -1;
  } else if (got == 14) {
    // 15 fields: the 1.14.48-1.15.1 BME pair.
    r.bme_t = btv > kAbsentBelow ? btv : NAN;
    r.bme_rh = bhv > kAbsentBelow ? bhv : NAN;
  } else if (got >= 15) {
    // 16 fields: 1.22.0 -- flips, then the bucket's measured draw range.
    r.flips = (btv >= 0.0f && btv <= 127.0f) ? static_cast<int8_t>(btv) : -1;
    r.w_min = bhv > kAbsentBelow ? bhv : NAN;
    r.w_max = wxv > kAbsentBelow ? wxv : NAN;
  }
  r.valid = true;
  return r;
}

}  // namespace storage
