// Regression suite from the 2026-08-11 dogfooding pass.
//
// Every case here started as a defect found by replaying real device payloads
// (captured from garage-fan at 1.14.22) through the console's own modules.
// They are kept as the guard against each one coming back.
//
// Cases still marked FAILS-TODAY document a defect that has not been fixed
// yet; they assert the correct behaviour so they fail until it is.

import { describe, expect, it } from 'vitest';
import { build, hasData } from '../src/series.js';
import { evaluate } from '../src/update.js';
import type { History } from '../src/types.js';
import type { Release } from '../src/update.js';

const END = 1786425931;
const STEP = 300;

/** /api/history?days=1 as the device returns it, now SD-backed and stamped. */
const RING_1D: History = {
  interval_s: STEP,
  source: 'sd',
  ts: Array.from({ length: 7 }, (_, i) => END - (6 - i) * STEP),
  temp_c: [25.0, 24.5, 24.5, 24.6, 24.1, 24.0, 24.0],
  rh: [37, 38, 37, 38, 39, 38, 39],
  hpa: [1000.0, 1000.0, 1000.0, 1000.0, 1000.0, 1000.0, 999.9],
  out_f: [null, 74.1, 74.1, 73.6, 73.6, 73.2, 73.2],
  batt_v: [4.2, 4.21, 4.2, 4.2, 4.22, 4.21, 4.2],
  spd: [5, 9, 7, 9, 9, 9, 9],
  chg: [1, 1, 1, 1, 1, 1, 1],
  watts: [12.4, 20.3, 15.4, 20.3, 20.1, 20.6, 20.3],
  voc_raw: [30100, 30125, 30140, 30160, 30150, 30170, 30190],
  nox_raw: [15800, 15805, 15810, 15810, 15815, 15820, 15820],
  voc: [0, 0, 0, 93, 95, 97, 96],
  nox: [0, 0, 0, 1, 1, 1, 1],
};

/**
 * The 7-day response. It now carries the same shape as every other range:
 * real per-row timestamps and all seven series. It used to carry interval_s
 * plus temp/rh/hpa and nothing else.
 */
const SD_7D: History = {
  interval_s: STEP,
  source: 'sd',
  // Two days of real samples inside a 7-day request, with an outage in the
  // middle -- the case the old fabricated interval could not represent.
  ts: [END - 172800, END - 172500, END - 86400, END - 86100],
  temp_c: [24.0, 25.5, 26.0, 26.5],
  rh: [40, 38, 36, 35],
  hpa: [1000.0, 999.5, 999.0, 998.5],
  out_f: [70.1, 71.2, 72.3, 73.4],
  batt_v: [4.1, 4.12, 4.15, 4.18],
  spd: [3, 5, 7, 9],
  chg: [1, 1, 0, 0],
  // Old rows: the meter and the air chain did not exist yet.
  watts: [null, null, null, null],
  voc_raw: [null, null, null, null],
  nox_raw: [null, null, null, null],
  voc: [null, null, null, null],
  nox: [null, null, null, null],
};

describe('7/30-day history now matches the 24h shape', () => {
  it('carries a real time axis', () => {
    const s = build(SD_7D);
    expect(s.ts(0)).toBe(END - 172800);
    expect(s.ts(3)).toBe(END - 86100);
  });

  it('keeps the outdoor / speed / battery series on the long ranges', () => {
    const s = build(SD_7D);
    expect(hasData(s.of)).toBe(true);
    expect(s.spd.length).toBe(s.n);
    expect(hasData(s.bv)).toBe(true);
  });

  it('detects the outage instead of drawing a continuous line across it', () => {
    // The old response reported interval_s = days*86400/rows, so every step
    // measured exactly one interval and no gap could ever be flagged.
    const s = build(SD_7D);
    expect(s.gap).toEqual([false, false, true, false]);
  });

  it('positions samples by real time, not by index', () => {
    const s = build(SD_7D);
    // Two clusters a day apart: the pair inside each cluster must sit far
    // closer together than the jump between them.
    expect(s.frac[1]! - s.frac[0]!).toBeLessThan(0.01);
    expect(s.frac[2]! - s.frac[1]!).toBeGreaterThan(0.9);
  });
});

describe('24h history', () => {
  it('places samples on a real clock', () => {
    const s = build(RING_1D);
    expect(s.ts(6)).toBe(END);
    expect(s.ts(0)).toBe(END - 6 * STEP);
  });

  it('spans a reboot without losing the samples before it', () => {
    // days=1 used to be hardwired to the RAM ring, which empties on every
    // reboot; the browser's localStorage cache was the only thing hiding it,
    // and only in the one browser that had been left open. Card-backed, the
    // rows from before an outage are still there and the hole is visible --
    // which is behaviour, unlike asserting the fixture's own `source` field.
    const across: History = {
      ...RING_1D,
      ts: [END - 7200, END - 6900, END - 600, END - 300, END],
      temp_c: [24, 24.2, 25, 25.1, 25.2],
      rh: [40, 40, 41, 41, 41],
      hpa: [1000, 1000, 999, 999, 999],
      out_f: [70, 70, 71, 71, 71],
      batt_v: [4.1, 4.1, 4.2, 4.2, 4.2],
      spd: [3, 3, 9, 9, 9],
      chg: [1, 1, 1, 1, 1],
    };
    const s = build(across);
    expect(s.n).toBe(5);
    expect(s.ts(0)).toBe(END - 7200); // pre-reboot rows survived
    expect(s.gap).toEqual([false, false, true, false, false]); // the dark stretch shows
  });
});

describe('update check against the live release feed', () => {
  const rel = (tag: string, assets: Release['assets'] = []): Release => ({
    tag_name: tag,
    name: tag,
    html_url: `https://github.com/jtn0123/ESP32-Garage-Fan/releases/tag/${tag}`,
    body: null,
    draft: false,
    prerelease: false,
    assets,
  });

  const bin = (tag: string) => ({
    name: `garage-fan-${tag}.bin`,
    browser_download_url: `https://example.invalid/${tag}.bin`,
    size: 1_100_000,
  });

  it('does not claim "up to date" when the device is AHEAD of the feed', () => {
    // The device ran 1.14.22 against a newest published release of v1.14.0.
    // The console rendered a green "up to date (v1.14.0)", which reads as
    // "the release channel is healthy" when it had been dead for 22 versions.
    const s = evaluate('1.14.22', [rel('v1.14.0', [bin('v1.14.0')]), rel('v1.13.0')]);
    expect(s.kind).toBe('ahead');
  });

  it('offers the newest stable release even when tagged out of order', () => {
    const s = evaluate('1.13.0', [rel('v1.13.1', [bin('v1.13.1')]), rel('v1.14.0', [bin('v1.14.0')])]);
    expect(s.kind).toBe('available');
    if (s.kind === 'available') expect(s.latest).toBe('v1.14.0');
  });

  it('does not offer a release with no attached binary as installable', () => {
    // This used to render a green "available" badge with a muted "no binary
    // attached yet" beside it -- an update the operator cannot act on, shown
    // with the same prominence as one they can. `available` now guarantees a
    // non-null asset by construction.
    const s = evaluate('1.13.0', [rel('v1.14.0')]);
    expect(s.kind).toBe('no-binary');
  });
});
