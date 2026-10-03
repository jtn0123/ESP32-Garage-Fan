// The winter limits on the temperature chart: which ones are drawn, when the
// y-range grows to take one in versus an edge marker, and where the tags sit.

import { describe, expect, it } from 'vitest';
import {
  type Box,
  chartLimits,
  collisions,
  extent,
  limitReach,
  pickBox,
  placeLimit,
} from '../src/limit_lines.js';
import { LIMIT_COLOURS } from '../src/theme.js';
import type { DeviceState } from '../src/types.js';

const state = (over: Partial<DeviceState>): DeviceState =>
  ({ floor_on: false, floor_f: 64, start_on: false, start_f: 74, ...over }) as DeviceState;

describe('chartLimits', () => {
  it('draws only the limits that are switched on', () => {
    expect(chartLimits(state({}))).toEqual([]);
    expect(chartLimits(state({ floor_on: true })).map((l) => l.kind)).toEqual(['floor']);
    expect(chartLimits(state({ start_on: true, floor_on: true })).map((l) => l.kind)).toEqual([
      'floor',
      'start',
    ]);
    expect(chartLimits(null)).toEqual([]);
  });

  it('gives each limit its own colour and name', () => {
    const [floor, start] = chartLimits(state({ floor_on: true, start_on: true, floor_f: 66 }));
    expect(floor).toMatchObject({ f: 66, name: 'LOW LIMIT', colour: LIMIT_COLOURS.floor });
    expect(start).toMatchObject({ f: 74, name: 'START', colour: LIMIT_COLOURS.start });
  });
});

describe('placeLimit: grow the range, or mark the edge', () => {
  it('always draws a limit that sits inside the data', () => {
    expect(placeLimit(74, 69, 79)).toBe('line');
  });

  it('grows the range for a limit within half the data span', () => {
    // A 10 °F day reaches 5 °F: a 66° limit under 69..79 is the approach.
    expect(limitReach(69, 79)).toBe(5);
    expect(placeLimit(66, 69, 79)).toBe('line');
    expect(placeLimit(83, 69, 79)).toBe('line');
  });

  it('marks the edge for a limit further out than that', () => {
    // A summer day with a 50° winter limit would otherwise be a flat strip.
    expect(placeLimit(50, 69, 79)).toBe('below');
    expect(placeLimit(95, 69, 79)).toBe('above');
  });

  it('marks the TOP edge for a low limit the garage sits under all day', () => {
    // The deep-winter case: the limit is above every reading.
    expect(placeLimit(64, 52, 56)).toBe('above');
  });

  it('reaches at least 3 °F on a calm day', () => {
    expect(limitReach(70, 71)).toBe(3);
    expect(placeLimit(67.5, 70, 71)).toBe('line');
  });
});

describe('extent', () => {
  it('skips gaps and reports null for nothing', () => {
    expect(extent([null, 70, 68.5, null, 72])).toEqual({ mn: 68.5, mx: 72 });
    expect(extent([null, null])).toBeNull();
  });
});

describe('tag placement', () => {
  const box = (x0: number, y0: number): Box => ({ x0, x1: x0 + 60, y0, y1: y0 + 15 });

  it('counts the data columns a tag would cover', () => {
    const cols = [
      { x: 10, y0: 100, y1: 140 }, // inside the box's span, overlapping it
      { x: 30, y0: 0, y1: 20 }, // inside the span, well above it
      { x: 200, y0: 100, y1: 140 }, // outside the span
    ];
    expect(collisions(box(0, 110), cols, [])).toBe(1);
  });

  it('takes the first candidate the traces leave clear', () => {
    const cols = [{ x: 20, y0: 100, y1: 130 }];
    const covered = box(0, 105);
    const clear = box(300, 105);
    expect(pickBox([covered, clear], cols, [])).toBe(clear);
  });

  it('never stacks a tag on one already placed', () => {
    const first = box(0, 50);
    const second = box(0, 52);
    const elsewhere = box(300, 52);
    expect(pickBox([second, elsewhere], [], [first])).toBe(elsewhere);
  });

  it('falls back to the least covered candidate', () => {
    const cols = [
      { x: 5, y0: 0, y1: 300 },
      { x: 25, y0: 0, y1: 300 },
      { x: 305, y0: 0, y1: 300 },
    ];
    const a = box(0, 100);
    const b = box(300, 100);
    expect(pickBox([a, b], cols, [])).toBe(b);
  });
});
