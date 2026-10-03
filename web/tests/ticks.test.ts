// Where the gridlines go (ticks.ts), and what limits() hands the rows.
//
// The y half is pinned against the before/after of the honest-ticks change:
// every example below used to label the padded min / mid / max -- 0.0 / 11.7 /
// 23.4 W, 3.86 / 4.05 / 4.24 V -- and now has to land on round values. The
// time half is pinned in LOCAL wall time, because that is what the labels and
// the night shading are read in.

import { afterEach, describe, expect, it } from 'vitest';
import { limits } from '../src/charts.js';
import {
  DAY,
  TIGHT_PX,
  decimals,
  minorStep,
  niceScale,
  resolution,
  timeStep,
  timeTicks,
} from '../src/ticks.js';

const HOUR = 3600;
/** A 66 px row's plot band, and the temperature chart's at desktop width. */
const ROW = 50;
const TALL = 234;

describe('limits: gridlines on round values', () => {
  it('labels watts 0 / 10 / 20, not 0.0 / 11.7 / 23.4', () => {
    const sc = limits([0.4, 4, 20.9], 2, 0, ROW)!;
    expect(sc.ticks).toEqual([0, 10, 20]);
    expect(sc.dp).toBe(0);
  });

  it('labels a LiPo 3.9 / 4.0 / 4.1 / 4.2, not 3.86 / 4.05 / 4.24', () => {
    const sc = limits([3.897, 4.05, 4.203], 0.1, -Infinity, ROW)!;
    expect(sc.ticks).toEqual([3.9, 4, 4.1, 4.2]);
    expect(sc.dp).toBe(1);
  });

  it('reaches a round value just past a quiet pressure trace', () => {
    // 999.6-999.8 widens to the 0.5 hPa floor; both ends are within reach of
    // a 0.2 step, so the row reads four lines instead of two.
    const sc = limits([999.6, 999.7, 999.8], 0.5, -Infinity, ROW)!;
    expect(sc.ticks).toEqual([999.4, 999.6, 999.8, 1000]);
    expect(sc.dp).toBe(1);
  });

  it('labels humidity and the gas index in fives and fifties', () => {
    expect(limits([39.5, 50.5], 2, 0, ROW)!.ticks).toEqual([40, 45, 50]);
    expect(limits([0, 61, 109], 2, 0, ROW)!.ticks).toEqual([0, 50, 100]);
  });

  it('gives the tall temperature chart up to five lines', () => {
    expect(limits([69, 74, 79], 2, -Infinity, TALL)!.ticks).toEqual([70, 72, 74, 76, 78]);
  });

  it('never ticks finer than the row can label', () => {
    // A flat 40.3 %RH night: half-percent lines would be true, but the 2-unit
    // floor exists so that range is never drawn as if it mattered.
    const sc = limits([40.3, 40.3], 2, 0, ROW)!;
    expect(sc.ticks.every(Number.isInteger)).toBe(true);
    expect(sc.ticks.length).toBeGreaterThanOrEqual(3);
  });

  it('keeps the floor at zero for quantities that stop there', () => {
    const flat = limits([0, 0, 0], 2, 0, ROW)!;
    expect(flat.min).toBe(0);
    expect(flat.ticks[0]).toBe(0);
  });

  it('copes with the raw gas ticks, five digits wide', () => {
    // Step 1 would be a 500-line grid; it must be skipped, not built.
    const sc = limits([29734, 30212], 2, 0, ROW)!;
    expect(sc.ticks.length).toBeGreaterThanOrEqual(2);
    expect(sc.ticks.length).toBeLessThanOrEqual(4);
    expect(sc.ticks.every((t) => t % 100 === 0)).toBe(true);
  });
});

describe('niceScale: invariants across a sweep of ranges', () => {
  // Spans from a sliver to five decades, offsets that put the data on and off
  // the grid -- the cases a hand-picked example list would miss.
  const cases: [number, number][] = [];
  for (const span of [0.13, 0.3, 0.62, 1, 2.2, 3.7, 6.6, 9.2, 14.8, 23, 55, 120, 480, 3000]) {
    for (const off of [0, 0.17, 0.5, 0.93, 3.31]) cases.push([off * span, off * span + span]);
  }

  it.each(cases)('[%f, %f] on a 66 px row', (lo, hi) => {
    const sc = niceScale(lo, hi, resolution(hi - lo < 1 ? 0.1 : 2), ROW);
    // Holds every value it was asked to.
    expect(sc.min).toBeLessThanOrEqual(lo);
    expect(sc.max).toBeGreaterThanOrEqual(hi);
    // Two to four lines, inside the range, legibly apart.
    expect(sc.ticks.length).toBeGreaterThanOrEqual(2);
    expect(sc.ticks.length).toBeLessThanOrEqual(4);
    for (const t of sc.ticks) {
      expect(t).toBeGreaterThanOrEqual(sc.min - 1e-9);
      expect(t).toBeLessThanOrEqual(sc.max + 1e-9);
    }
    const px = ((sc.ticks[1]! - sc.ticks[0]!) / (sc.max - sc.min)) * ROW;
    expect(px).toBeGreaterThanOrEqual(TIGHT_PX - 1e-9);
    // Labels at the scale's decimals are all distinct.
    const labels = sc.ticks.map((t) => t.toFixed(sc.dp));
    expect(new Set(labels).size).toBe(labels.length);
  });
});

describe('resolution and decimals', () => {
  it('reads the label step back off the minimum span', () => {
    expect(resolution(2)).toBe(1);
    expect(resolution(0.5)).toBe(0.2);
    expect(resolution(0.1)).toBe(0.05);
  });

  it('gives a step the decimals its multiples need', () => {
    expect(decimals(50)).toBe(0);
    expect(decimals(5)).toBe(0);
    expect(decimals(1)).toBe(0);
    expect(decimals(0.5)).toBe(1);
    expect(decimals(0.2)).toBe(1);
    expect(decimals(0.1)).toBe(1);
    expect(decimals(0.05)).toBe(2);
  });
});

describe('timeStep: label spacing per range', () => {
  const DESK = 1082; // 1136 px chart minus the gutters
  const PHONE = 326; // Pixel 7
  const pick = (spanS: number, w: number): number => timeStep(spanS, w, w > 600 ? 100 : 60);

  it('gives a desktop round steps that never crowd', () => {
    expect(pick(6 * HOUR, DESK)).toBe(HOUR);
    expect(pick(12 * HOUR, DESK)).toBe(2 * HOUR);
    expect(pick(24 * HOUR, DESK)).toBe(3 * HOUR);
    expect(pick(7 * DAY, DESK)).toBe(DAY);
    expect(pick(30 * DAY, DESK)).toBe(7 * DAY);
    expect(pick(60 * DAY, DESK)).toBe(7 * DAY);
  });

  it('thins them out on a phone', () => {
    expect(pick(6 * HOUR, PHONE)).toBe(2 * HOUR);
    expect(pick(24 * HOUR, PHONE)).toBe(6 * HOUR);
    expect(pick(7 * DAY, PHONE)).toBe(2 * DAY);
    expect(pick(30 * DAY, PHONE)).toBe(7 * DAY);
    expect(pick(60 * DAY, PHONE)).toBe(14 * DAY);
  });

  it('falls back to the coarsest step rather than overlapping', () => {
    expect(timeStep(365 * DAY, 300, 60)).toBe(14 * DAY);
  });
});

describe('minorStep: unlabelled marks between labels', () => {
  it('counts hours between 3-hourly labels and days between weekly ones', () => {
    expect(minorStep(3 * HOUR, 24 * HOUR, 1082)).toBe(HOUR);
    expect(minorStep(7 * DAY, 30 * DAY, 1082)).toBe(DAY);
  });

  it('steps up, or gives up, when the marks would crowd', () => {
    expect(minorStep(6 * HOUR, 24 * HOUR, 326)).toBe(3 * HOUR);
    expect(minorStep(7 * DAY, 30 * DAY, 326)).toBeNull();
  });
});

describe('timeTicks: round local clock times', () => {
  const local = (mo: number, d: number, h = 0, m = 0): number =>
    Math.floor(new Date(2026, mo - 1, d, h, m).getTime() / 1000);
  const date = (t: number): Date => new Date(t * 1000);

  it('puts 24 h ticks on the 3-hour marks, not on sample times', () => {
    // The old axis labelled every Nth row: 20:46, 23:26, 02:06...
    const ticks = timeTicks(local(9, 30, 20, 46), local(10, 1, 20, 46), 3 * HOUR);
    expect(ticks.map((t) => date(t).getHours())).toEqual([21, 0, 3, 6, 9, 12, 15, 18]);
    expect(ticks.every((t) => date(t).getMinutes() === 0)).toBe(true);
  });

  it('includes an instant exactly at either end', () => {
    const ticks = timeTicks(local(9, 30, 21), local(10, 1, 0), 3 * HOUR);
    expect(ticks).toEqual([local(9, 30, 21), local(10, 1, 0)]);
  });

  it('puts daily ticks on midnights', () => {
    const ticks = timeTicks(local(9, 25, 21), local(10, 2, 21), DAY);
    expect(ticks).toHaveLength(7);
    expect(ticks.every((t) => date(t).getHours() === 0)).toBe(true);
    expect(date(ticks[0]!).getDate()).toBe(26);
  });

  it('puts two-day ticks two calendar days apart', () => {
    const ticks = timeTicks(local(9, 25, 21), local(10, 2, 21), 2 * DAY);
    const days = ticks.map((t) => Math.round((t - ticks[0]!) / DAY));
    expect(days).toEqual([0, 2, 4, 6].slice(0, days.length));
    expect(ticks.length).toBeGreaterThanOrEqual(3);
  });

  it('puts weekly and fortnightly ticks on Mondays', () => {
    for (const step of [7 * DAY, 14 * DAY]) {
      const ticks = timeTicks(local(8, 3, 21), local(10, 2, 21), step);
      expect(ticks.length).toBeGreaterThan(0);
      expect(ticks.every((t) => date(t).getDay() === 1 && date(t).getHours() === 0)).toBe(true);
    }
  });

  it('is empty for a window with no span', () => {
    expect(timeTicks(local(9, 30, 12), local(9, 30, 12), HOUR)).toEqual([]);
    expect(timeTicks(local(9, 30, 12), local(9, 30, 11), HOUR)).toEqual([]);
  });
});

describe('timeTicks across a DST change', () => {
  // Node re-reads TZ on assignment, so the zone can be pinned per test: the
  // box running CI may well have no DST at all.
  const saved = process.env['TZ'];
  afterEach(() => {
    if (saved === undefined) delete process.env['TZ'];
    else process.env['TZ'] = saved;
  });

  it('keeps 3-hourly ticks on 00/03/06 local through spring-forward', () => {
    process.env['TZ'] = 'America/Los_Angeles';
    const t0 = new Date(2026, 2, 7, 22).getTime() / 1000; // Sat 7 Mar 22:00 PST
    const tn = new Date(2026, 2, 8, 13).getTime() / 1000; // Sun 8 Mar 13:00 PDT
    const hours = timeTicks(t0, tn, 3 * HOUR).map((t) => new Date(t * 1000).getHours());
    expect(hours).toEqual([0, 3, 6, 9, 12]);
  });

  it('does not stack two hourly ticks on the hour that does not exist', () => {
    process.env['TZ'] = 'America/Los_Angeles';
    const t0 = new Date(2026, 2, 8, 0).getTime() / 1000;
    const tn = new Date(2026, 2, 8, 5).getTime() / 1000;
    const ticks = timeTicks(t0, tn, HOUR);
    expect(new Set(ticks).size).toBe(ticks.length);
    expect(ticks.map((t) => new Date(t * 1000).getHours())).toEqual([0, 1, 3, 4, 5]);
  });
});
