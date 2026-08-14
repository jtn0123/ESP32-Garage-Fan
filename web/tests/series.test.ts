import { describe, expect, it } from 'vitest';
import { build, hasData } from '../src/series.js';
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
  ...over,
});

describe('build', () => {
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
