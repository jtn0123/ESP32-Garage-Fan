// Turning /api/history into something the charts can draw.
//
// One job now: normalise the wire format (Celsius -> Fahrenheit, the -999
// outdoor sentinel -> null, short series padded to length).
//
// The second job this module used to have -- merging the device's 24 h window
// with a localStorage copy so a reboot did not blank the chart -- is gone.
// That cache existed only because /api/history?days=1 was hardwired to the
// device's RAM ring, which empties on every reboot; the firmware now serves
// all three ranges from the SD card, so the card is the record and the browser
// does not need to keep a shadow copy of it. Removing it also removes what
// that cache got wrong: it was keyed on the constant 'gf24' with no device
// identity, so a browser that opened two fans merged one's readings into the
// other's chart.

import type { History } from './types.js';

export interface Series {
  n: number;
  /** Epoch seconds for sample i, or null before SNTP has synced. */
  ts: (i: number) => number | null;
  /**
   * Horizontal position of sample i as a 0..1 fraction, proportional to TIME:
   * an outage occupies its true width on the axis instead of being squeezed
   * to one sample step. Index-proportional before SNTP has synced.
   */
  frac: number[];
  /**
   * True where the step from sample i-1 to i spans more than ~1.5 intervals --
   * the device was dark. Chart lines break here rather than fabricating a
   * bridge across the hole.
   */
  gap: boolean[];
  night: boolean[];
  tf: (number | null)[]; // garage, Fahrenheit
  of: (number | null)[]; // outside, Fahrenheit
  rh: (number | null)[];
  hpa: (number | null)[];
  spd: number[];
  bv: (number | null)[];
  chg: number[];
}

/**
 * The firmware's CSV path stores -999 when no outdoor reading existed; treat
 * anything at or below this threshold as absent rather than matching the
 * sentinel exactly, so a float round-trip cannot sneak it past the filter.
 */
const OUTDOOR_ABSENT_MAX = -100;

/**
 * Pad a series to `n`, so a response whose columns disagree in length cannot
 * put `undefined` into the charts.
 *
 * build() takes its length from temp_c and used to pass every other array
 * through untouched. Any short series then read undefined past its end and
 * plotted as NaN -- which is exactly what the old 7/30-day response did by
 * omitting four series entirely.
 */
function pad<T>(a: readonly T[] | undefined, n: number, fill: T): T[] {
  const out = (a ?? []).slice(0, n) as T[];
  while (out.length < n) out.push(fill);
  return out;
}

export function build(h: History): Series {
  const n = h.temp_c?.length ?? 0;
  // Real per-row epochs from the device. A 0 means SNTP had not synced when
  // that row was taken, which is "unknown", not 1970.
  const stamps = pad<number>(h.ts, n, 0);
  const ts = (i: number): number | null => {
    const v = stamps[i];
    return v ? v : null;
  };

  const t0 = ts(0);
  const tn = ts(n - 1);
  const span = t0 !== null && tn !== null ? tn - t0 : 0;
  const frac: number[] = [];
  const gap: boolean[] = [];
  for (let i = 0; i < n; i++) {
    const t = ts(i);
    frac.push(
      span > 0 && t0 !== null && t !== null ? (t - t0) / span : n > 1 ? i / (n - 1) : 0,
    );
    const prev = i > 0 ? ts(i - 1) : null;
    gap.push(prev !== null && t !== null && t - prev > h.interval_s * 1.5);
  }

  const night: boolean[] = [];
  for (let i = 0; i < n; i++) {
    const t = ts(i);
    if (t === null) {
      night.push(false);
      continue;
    }
    const hr = new Date(t * 1000).getHours();
    night.push(hr >= 20 || hr < 6);
  }

  return {
    n,
    ts,
    frac,
    gap,
    night,
    tf: pad<number | null>(h.temp_c, n, null).map((v) => (v === null ? null : (v * 9) / 5 + 32)),
    of: pad<number | null>(h.out_f, n, null).map((v) =>
      v === null || v <= OUTDOOR_ABSENT_MAX ? null : v,
    ),
    rh: pad<number | null>(h.rh, n, null),
    hpa: pad<number | null>(h.hpa, n, null),
    spd: pad<number>(h.spd, n, 0),
    bv: pad<number | null>(h.batt_v, n, null),
    chg: pad<number>(h.chg, n, -1),
  };
}

/** Does a series carry at least one real reading? */
export function hasData(series: readonly (number | null)[] | undefined): boolean {
  return !!series && series.some((v) => v !== null && !Number.isNaN(v));
}
