import { describe, expect, it } from 'vitest';
import { build, hasData, tail } from '../src/series.js';
import type { History } from '../src/types.js';

const T = 1_786_071_600;

const h = (over: Partial<History> = {}): History => ({
  interval_s: 300,
  source: 'sd',
  ts: [T - 600, T - 300, T],
  temp_c: [20, 21, 22],
  rh: [50, 51, 52],
  hpa: [1010, 1011, 1012],
  out_f: [70, -999, 72],
  spd: [0, 9, 9],
  batt_v: [3.9, null, 4.0],
  chg: [0, 1, 1],
  watts: [null, 20.3, 21.0],
  voc_raw: [null, 30125, 30200],
  nox_raw: [null, 15800, 15810],
  voc: [null, 0, 87],
  nox: [null, 0, 1],
  flips: [null, 0, 3],
  w_min: [null, 20.1, 4.3],
  w_max: [null, 20.5, 45.6],
  ...over,
});

describe('build', () => {
  it('carries the bucket draw range the power band shades', () => {
    // The half a single `watts` snapshot cannot carry: inside the last bucket
    // the meter swung 4.3 to 45.6 W, which is a fan stopping and restarting.
    const s = build(h());
    expect(s.wmin).toEqual([null, 20.1, 4.3]);
    expect(s.wmax).toEqual([null, 20.5, 45.6]);
    // A pre-1.22.0 firmware sends neither: nulls, not a throw.
    const old = h();
    delete (old as Partial<History>).w_min;
    delete (old as Partial<History>).w_max;
    expect(build(old).wmin).toEqual([null, null, null]);
    expect(build(old).wmax).toEqual([null, null, null]);
  });

  it('carries the plug flip count per row, null where the meter had none', () => {
    // The cycling profile's column; the power chart tints any bucket > 0.
    expect(build(h()).flips).toEqual([null, 0, 3]);
    // A pre-1.21.0 firmware sends no flips at all: every row is null, not a throw.
    const old = h();
    delete (old as Partial<History>).flips;
    expect(build(old).flips).toEqual([null, null, null]);
  });

  it('converts garage temperature to Fahrenheit', () => {
    const s = build(h());
    expect(s.tf[0]).toBeCloseTo(68);
    expect(s.tf[2]).toBeCloseTo(71.6);
  });

  it('normalises the -999 outdoor sentinel to null', () => {
    const s = build(h());
    expect(s.of).toEqual([70, null, 72]);
  });

  it('reads timestamps straight off the wire, newest last', () => {
    const s = build(h());
    expect(s.ts(2)).toBe(T);
    expect(s.ts(0)).toBe(T - 600);
  });

  it('treats a 0 timestamp as unknown rather than 1970', () => {
    const s = build(h({ ts: [0, 0, 0] }));
    expect(s.ts(0)).toBeNull();
    expect(s.night.every((v) => v === false)).toBe(true);
  });

  it('spaces uniformly and flags no gaps for evenly spaced samples', () => {
    const s = build(h());
    expect(s.frac).toEqual([0, 0.5, 1]);
    expect(s.gap).toEqual([false, false, false]);
  });

  it('positions samples by real time and flags the hole across an outage', () => {
    const base = 1_786_000_000;
    // Three 5-min samples, then the device was dark for two hours.
    const gappy = h({
      ts: [base, base + 300, base + 600, base + 7800],
      temp_c: [20, 21, 22, 23],
      rh: [50, 51, 52, 53],
      hpa: [1010, 1011, 1012, 1013],
      out_f: [70, 71, 72, 73],
      spd: [0, 9, 9, 3],
      batt_v: [3.9, 3.9, 3.9, 3.9],
      chg: [0, 0, 0, 0],
    });
    const s = build(gappy);
    expect(s.ts(0)).toBe(base);
    expect(s.ts(3)).toBe(base + 7800);
    // The outage occupies its true width: the first three points huddle at
    // the left, the fourth sits at the far edge.
    expect(s.frac[1]).toBeCloseTo(300 / 7800);
    expect(s.frac[2]).toBeCloseTo(600 / 7800);
    expect(s.frac[3]).toBe(1);
    expect(s.gap).toEqual([false, false, false, true]);
  });

  it('pads every series to temp_c length so short columns cannot plot NaN', () => {
    // build() takes n from temp_c. Before padding, any column the device sent
    // short read undefined past its end and reached the canvas as NaN.
    const s = build(h({ hpa: [1010], out_f: [70], spd: [1], batt_v: [3.9], chg: [1] }));
    expect(s.n).toBe(3);
    for (const col of [s.hpa, s.of, s.rh, s.bv, s.spd, s.chg]) {
      expect(col.length).toBe(3);
    }
    expect(s.hpa).toEqual([1010, null, null]);
    expect(s.chg).toEqual([1, -1, -1]);
  });

  it('ignores extra entries in a series longer than temp_c', () => {
    const s = build(h({ rh: [1, 2, 3, 4, 5] }));
    expect(s.rh).toEqual([1, 2, 3]);
  });
});

describe('hasData', () => {
  it('sees through nulls and NaNs', () => {
    expect(hasData([null, null, 3])).toBe(true);
    expect(hasData([null, NaN])).toBe(false);
    expect(hasData([])).toBe(false);
    expect(hasData(undefined)).toBe(false);
  });
});

/**
 * A wide range does not arrive at the sample cadence.
 *
 * /api/history reads the whole window off the card and decimates it to
 * kGraphMaxPts, so 7 days come back ~2400 s apart and 60 days ~18300 s --
 * while interval_s keeps reporting the device's 300 s SAMPLE cadence, which is
 * all the firmware ever promised it meant. Measuring the step against
 * interval_s therefore called every row of a long range an outage: every line
 * broke into invisible one-point segments and the whole chart shaded red,
 * which is what "7D/30D/60D show no data" actually was.
 */
describe('build: the row spacing is measured, not assumed', () => {
  const decimated = (step: number, n: number, gapAt = -1): History => {
    const ts: number[] = [];
    let t = T - (n - 1) * step;
    for (let i = 0; i < n; i++) {
      if (i === gapAt) t += step * 40;
      ts.push(t);
      t += step;
    }
    return h({
      interval_s: 300, // the device's cadence, unchanged by the range
      ts,
      temp_c: ts.map(() => 24),
      rh: ts.map(() => 40),
      hpa: ts.map(() => 1000),
      out_f: ts.map(() => 70),
      spd: ts.map(() => 3),
      batt_v: ts.map(() => 4),
      chg: ts.map(() => 0),
      watts: ts.map(() => 5),
      voc_raw: ts.map(() => 30000),
      nox_raw: ts.map(() => 15800),
      voc: ts.map(() => 90),
      nox: ts.map(() => 1),
    });
  };

  it('reads the real step off the timestamps', () => {
    expect(build(decimated(2400, 20)).step).toBe(2400);
  });

  it('does not call every row of a decimated range an outage', () => {
    expect(build(decimated(2400, 20)).gap.some(Boolean)).toBe(false);
  });

  it('still finds a genuine hole at the coarser step', () => {
    const s = build(decimated(2400, 20, 8));
    expect(s.gap[8]).toBe(true);
    expect(s.gap.filter(Boolean)).toHaveLength(1);
  });

  it('falls back to interval_s when no stamp has synced', () => {
    const n = 4;
    expect(build(h({ ts: [0, 0, 0, 0].slice(0, n), temp_c: [24, 24, 24, 24] })).step).toBe(300);
  });
});

/* ------------------------------------------------------------------ tail() */

/**
 * The 6 h and 12 h ranges are a window on the 24 h response, so the slicing is
 * the whole feature. Getting it wrong shows a chart whose axis says six hours
 * and whose data is a day.
 */
describe('tail', () => {
  const at = (n: number, first: number, step: number): History =>
    ({
      interval_s: step,
      source: 'sd',
      ts: Array.from({ length: n }, (_, i) => first + i * step),
      temp_c: Array.from({ length: n }, (_, i) => i),
      rh: Array.from({ length: n }, (_, i) => i),
      hpa: Array.from({ length: n }, (_, i) => i),
      out_f: Array.from({ length: n }, (_, i) => i),
      spd: Array.from({ length: n }, (_, i) => i),
      batt_v: Array.from({ length: n }, (_, i) => i),
      chg: Array.from({ length: n }, () => 0),
      watts: Array.from({ length: n }, (_, i) => i),
      voc_raw: Array.from({ length: n }, (_, i) => i),
    }) as unknown as History;

  it('keeps only the requested hours', () => {
    const h = at(288, 1_700_000_000, 300); // 24 h at 5 min
    const six = tail(h, 6);
    expect(six.ts.length).toBe(73); // 6 h, inclusive of the boundary row
    expect(six.ts[six.ts.length - 1]).toBe(h.ts[287]);
  });

  it('slices every series to the same length', () => {
    const six = tail(at(288, 1_700_000_000, 300), 6);
    const lengths = Object.values(six)
      .filter(Array.isArray)
      .map((a) => (a as unknown[]).length);
    expect(new Set(lengths).size).toBe(1);
  });

  it('returns the response untouched when it is already shorter', () => {
    const h = at(10, 1_700_000_000, 300); // 50 min of rows
    expect(tail(h, 12)).toBe(h);
  });

  it('falls back to the cadence when the clock has not synced', () => {
    const h = at(288, 0, 300);
    (h as { ts: number[] }).ts = Array.from({ length: 288 }, () => 0);
    expect(tail(h, 6).ts.length).toBe(72); // 6 h / 5 min, counted not timed
  });

  it('cuts by time, not by row count, across an outage', () => {
    // Six rows over 12 h: a dense hour, then a long dark stretch. Asking for
    // the last 2 h must keep only what actually falls inside it.
    const h = at(6, 1_700_000_000, 300);
    (h as { ts: number[] }).ts = [0, 300, 600, 900, 40_000, 43_200].map(
      (o) => 1_700_000_000 + o,
    );
    const two = tail(h, 2);
    expect(two.ts.length).toBe(2);
    expect(two.temp_c.length).toBe(2);
  });
});
