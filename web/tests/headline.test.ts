// The one-word fan state over the rail, including the winter and bad-air
// states summer hardware never shows: the low limit, the start gate, the gas
// boost -- and the order they outrank each other in.

import { describe, expect, it } from 'vitest';
import { fanHeadline, headlineLabel, spinSeconds } from '../src/headline.js';
import type { DeviceState } from '../src/types.js';

const base = (over: Partial<DeviceState>): DeviceState =>
  ({
    speed: 9,
    auto: true,
    auto_max: 9,
    auto_min: 0,
    on_f: 2.5,
    off_f: 1.5,
    floor_on: false,
    floor_f: 64,
    start_on: false,
    start_f: 74,
    limit: null,
    outside_f: 70,
    gas_on: true,
    gas_spd: 6,
    gas_active: false,
    ...over,
  }) as DeviceState;

describe('fanHeadline: the thermostat', () => {
  it('says VENTING while auto runs above its rest speed', () => {
    const h = fanHeadline(base({}));
    expect([h.word, h.tone, h.speed]).toEqual(['VENTING', 'run', 'speed 9']);
  });

  it('says RESTING at the rest speed, stopped or not', () => {
    expect(fanHeadline(base({ speed: 0 })).word).toBe('RESTING');
    expect(fanHeadline(base({ speed: 0 })).speed).toBe('fan off');
    const slow = fanHeadline(base({ speed: 2, auto_min: 2 }));
    expect([slow.word, slow.tone, slow.speed]).toEqual(['RESTING', 'rest', 'speed 2']);
  });

  it('does not claim a verdict it does not have with no yard reading', () => {
    const h = fanHeadline(base({ outside_f: null }));
    expect([h.word, h.detail, h.tone]).toEqual(['HOLDING', 'NO OUTDOOR READING', 'hold']);
  });
});

describe('fanHeadline: the winter limits', () => {
  it('names the low limit over a running speed', () => {
    const h = fanHeadline(base({ limit: 'floor', speed: 0 }));
    expect([h.word, h.detail, h.tone, h.speed]).toEqual(['HELD', 'LOW LIMIT', 'floor', 'fan off']);
  });

  it('names the start point', () => {
    const h = fanHeadline(base({ limit: 'start', speed: 0 }));
    expect([h.word, h.detail, h.tone]).toEqual(['WAITING', 'START POINT', 'start']);
  });

  it('holds the low limit even without a yard reading, as the sentence does', () => {
    expect(fanHeadline(base({ limit: 'floor', outside_f: null })).word).toBe('HELD');
  });
});

describe('fanHeadline: the gas boost', () => {
  it('outranks a limit while it is what holds the fan up', () => {
    const h = fanHeadline(base({ gas_active: true, limit: 'floor', speed: 6 }));
    expect([h.word, h.tone, h.speed]).toEqual(['GAS BOOST', 'gas', 'speed 6']);
  });

  it('gives way to the thermostat once that wants more than the boost', () => {
    expect(fanHeadline(base({ gas_active: true, speed: 9, gas_spd: 6 })).word).toBe('VENTING');
  });

  it('is an auto-mode feature: manual mode ignores a stale latch', () => {
    expect(fanHeadline(base({ gas_active: true, auto: false, speed: 6 })).word).toBe('MANUAL');
  });
});

describe('fanHeadline: by hand and the edge cases', () => {
  it('says MANUAL with auto off and OFF with nothing running', () => {
    const hand = fanHeadline(base({ auto: false, speed: 5 }));
    expect([hand.word, hand.tone, hand.speed]).toEqual(['MANUAL', 'hand', 'speed 5']);
    const off = fanHeadline(base({ auto: false, speed: 0 }));
    expect([off.word, off.speed, off.spin]).toEqual(['OFF', null, null]);
  });

  it('never spins or names a step for a raw or unknown output', () => {
    for (const speed of [-1, -2]) {
      const h = fanHeadline(base({ speed }));
      expect(h.spin).toBeNull();
      expect(h.speed).toBeNull();
    }
    expect(fanHeadline(base({ speed: -1 })).word).toBe('RAW DUTY');
    expect(fanHeadline(base({ speed: -2 })).tone).toBe('fault');
  });

  it('puts an actuator fault above everything', () => {
    const h = fanHeadline(base({ actuator_fault: true, gas_active: true, limit: 'floor' }));
    expect([h.word, h.tone, h.spin]).toEqual(['FAULT', 'fault', null]);
  });
});

describe('spinSeconds', () => {
  it('is still at rest and quickens monotonically with speed', () => {
    expect(spinSeconds(0)).toBeNull();
    const turns = Array.from({ length: 12 }, (_, k) => spinSeconds(k + 1)!);
    for (let k = 1; k < turns.length; k++) expect(turns[k]).toBeLessThan(turns[k - 1]!);
    expect(turns[0]).toBe(2.7);
    expect(turns[11]).toBe(0.72);
  });
});

describe('headlineLabel', () => {
  it('joins only the parts that exist', () => {
    expect(headlineLabel(fanHeadline(base({ limit: 'floor', speed: 0 })))).toBe(
      'HELD · LOW LIMIT · fan off',
    );
    expect(headlineLabel(fanHeadline(base({ auto: false, speed: 0 })))).toBe('OFF');
  });
});
