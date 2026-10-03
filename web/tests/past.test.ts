// History mode: the words and values the hero shows for a logged moment, and
// how a pinned moment survives the history being rebuilt under it.
//
// The painting itself is pinned in the e2e suite (it needs layout); these are
// the pure decisions behind it.

import { describe, expect, it } from 'vitest';
import { pastLine, pastMetrics } from '../src/past.js';
import { pinnedRow } from '../src/state.js';

describe('pastLine: the moment the hero is describing', () => {
  const t = Math.floor(new Date(2026, 7, 16, 15, 27).getTime() / 1000); // a Sunday

  it('reads like the chart header readout, so the two never disagree', () => {
    expect(pastLine(t, 1, 9 * 60 + 50)).toBe('15:27 · 9H50 AGO');
    expect(pastLine(t, 7, 3 * 24 * 60)).toBe('SUN 8/16 15:27 · 3D AGO');
  });

  it('falls back to the age alone before the clock has synced', () => {
    expect(pastLine(null, 1, 45)).toBe('45 MIN AGO');
  });
});

describe('pastMetrics: the strip as it read at that moment', () => {
  // A 13-entry duty table like the device's: index 0 is OFF.
  const high = [0, 900, 1700, 2500, 3300, 4100, 4900, 5600, 6400, 7850, 8600, 9300, 9934];

  it('derives airflow and duty from the logged speed, draw from the plug', () => {
    expect(pastMetrics(9, 20.31, high, 9934)).toEqual({
      air: 'Strong',
      draw: '20.3 W',
      duty: '79.0%',
    });
  });

  it('says the fan was still, at zero duty, when it was logged off', () => {
    expect(pastMetrics(0, 0, high, 9934)).toEqual({ air: 'Still', draw: '0.0 W', duty: '0.0%' });
  });

  it('does not invent a stopped fan for a row with no speed in it', () => {
    // An unlogged speed is not "Still" -- the same rule as the sentence.
    const m = pastMetrics(undefined, null, high, 9934);
    expect(m).toEqual({ air: '–', draw: '–', duty: '–' });
  });

  it('leaves duty blank when the duty table has not arrived yet', () => {
    // /api/device is retried in the background; until it lands there is no
    // table to look the speed up in, and 0.0% would claim the line was idle.
    expect(pastMetrics(9, 20.3, undefined, 9934).duty).toBe('–');
  });
});

describe('pinnedRow: a held moment across a rebuilt series', () => {
  const STEP = 300;
  const series = (first: number, n: number) => ({
    n,
    step: STEP,
    ts: (i: number) => (i >= 0 && i < n ? first + i * STEP : null),
  });

  it('finds the same moment after the window slides on', () => {
    // The minute refresh drops the oldest rows: the pinned 10:00 sample moves
    // from index 20 to index 18, and the pin has to follow it, not the index.
    const before = series(1_000_000, 288);
    const pinTs = before.ts(20)!;
    const after = series(1_000_000 + 2 * STEP, 288);
    expect(pinnedRow(after, pinTs)).toBe(18);
  });

  it('lets go once the moment has left the window', () => {
    const s = series(1_000_000, 72);
    expect(pinnedRow(s, 1_000_000 - 10 * STEP)).toBe(-1);
  });

  it('is never pinned with nothing to pin', () => {
    expect(pinnedRow(series(1_000_000, 10), null)).toBe(-1);
    const noClock = { n: 10, step: STEP, ts: () => null };
    expect(pinnedRow(noClock, 1_000_000)).toBe(-1);
  });
});
