// The live "why" sentence, including the winter cases summer hardware never
// reaches: the low limit holding the fan at rest, and the start gate leaving
// a warm-but-not-hot garage alone.

import { describe, expect, it } from 'vitest';
import { FLOOR_RESUME_F, liveReason } from '../src/reason.js';
import type { DeviceState } from '../src/types.js';

const base = (over: Partial<DeviceState>): DeviceState =>
  ({
    speed: 0,
    auto: true,
    auto_max: 10,
    auto_min: 0,
    on_f: 2.5,
    off_f: 1.5,
    floor_on: true,
    floor_f: 64,
    start_on: true,
    start_f: 74,
    limit: null,
    outside_f: 30,
    gas_active: false,
    ...over,
  }) as DeviceState;

describe('liveReason: the low limit', () => {
  it('says the limit, not the 34-degree gap, is why the fan is off', () => {
    const text = liveReason(base({ limit: 'floor' }), 63.8);
    expect(text).toContain('64° low limit');
    expect(text).toContain('63.8°F');
    expect(text).toContain('keeps the fan off');
    expect(text).toContain(`above ${64 + FLOOR_RESUME_F}°`);
    expect(text).not.toContain('engage point');
  });

  it('names a non-zero rest speed rather than claiming the fan is off', () => {
    expect(liveReason(base({ limit: 'floor', auto_min: 2, speed: 2 }), 63)).toContain(
      'keeps the fan at speed 2',
    );
  });

  it('explains the resume margin once the garage is back above the limit', () => {
    const text = liveReason(base({ limit: 'floor' }), 64.8);
    expect(text).toContain('just above your 64° low limit');
    expect(text).toContain('waits for 65.5°');
  });

  it('does not contradict a running gas boost', () => {
    const text = liveReason(base({ limit: 'floor', gas_active: true, speed: 6 }), 63);
    expect(text).toContain('gas boost is still running');
  });

  it('holds even without an outdoor reading -- the limit needs only the garage', () => {
    const text = liveReason(base({ limit: 'floor', outside_f: null }), 63);
    expect(text).toContain('low limit');
    expect(text).not.toContain('No outdoor reading');
  });
});

describe('liveReason: the start gate', () => {
  it('says the garage is under the start point, gap or no gap', () => {
    const text = liveReason(base({ limit: 'start' }), 70);
    expect(text).toContain('40.0°F hotter than the yard');
    expect(text).toContain('under your 74° start point');
    expect(text).toContain('leaves the fan off');
  });
});

describe('liveReason: the differential, limits idle', () => {
  it('mentions the low limit as a second way out while venting', () => {
    const text = liveReason(base({ speed: 10 }), 70);
    expect(text).toContain('holding speed 10');
    expect(text).toContain('stops at once if the garage cools to your 64° low limit');
  });

  it('says nothing about limits that are switched off', () => {
    const text = liveReason(base({ speed: 10, floor_on: false, start_on: false }), 70);
    expect(text).not.toContain('low limit');
  });

  it('keeps the summer sentences for summer', () => {
    const summer = base({ floor_on: false, start_on: false, outside_f: 80 });
    expect(liveReason(summer, 80.5)).toContain('under the +1.5° release point');
    expect(liveReason(summer, 82)).toContain('inside the +1.5°/+2.5° deadband');
    expect(liveReason({ ...summer, auto: false }, 82)).toContain('Auto is off');
    expect(liveReason({ ...summer, outside_f: null }, 82)).toContain('No outdoor reading');
  });

  it('ignores a stale limit verdict once auto is off', () => {
    // The firmware nulls `limit` in manual mode; this is the belt to that brace.
    expect(liveReason(base({ auto: false, limit: 'floor' }), 63)).toContain('Auto is off');
  });
});
