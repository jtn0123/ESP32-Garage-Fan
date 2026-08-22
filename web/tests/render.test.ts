// Tests for the rendering-side logic that had none.
//
// charts.ts, console.ts and format.ts carried several of the sweep's fixes with
// zero automated coverage; every one of these was verified by hand against a
// mock device first, then pinned here.

import { describe, expect, it } from 'vitest';
import { limits } from '../src/charts.js';
import { isNewer } from '../src/console.js';
import { battReadout, powerReadout, speedReadout } from '../src/history_view.js';
import { plugBit, plugColour } from '../src/status_bits.js';
import { axisLabel, cardTight, storage } from '../src/format.js';
import type { DeviceState } from '../src/types.js';

describe('limits: minimum y-span', () => {
  it('does not magnify a flat series', () => {
    // A calm night at 40.1-40.5 %RH used to fill the whole chart height while
    // all three gridlines read "40" -- the axis said nothing was happening and
    // the line said everything was, and the line is what gets read.
    const sc = limits([40.1, 40.2, 40.5, 40.3])!;
    expect(sc.max - sc.min).toBeGreaterThanOrEqual(2);
  });

  it('centres the floor on the data rather than pinning it to an edge', () => {
    const sc = limits([40, 40, 40])!;
    const mid = (sc.min + sc.max) / 2;
    expect(mid).toBeCloseTo(40, 6);
  });

  it('leaves a genuinely wide series alone', () => {
    const sc = limits([10, 90])!;
    expect(sc.min).toBeLessThan(10);
    expect(sc.max).toBeGreaterThan(90);
    expect(sc.max - sc.min).toBeGreaterThan(80);
  });

  it('takes a per-series floor, so battery is not flattened', () => {
    // A LiPo's entire working range is ~0.7 V. The 2-unit default would render
    // every real discharge curve as a straight line.
    const sc = limits([3.95, 4.05, 4.2], 0.1)!;
    expect(sc.max - sc.min).toBeLessThan(0.5);
  });

  it('returns null when there is nothing to scale', () => {
    expect(limits([])).toBeNull();
    expect(limits([null, null])).toBeNull();
  });

  it('never produces a zero-height scale (no divide-by-zero in yAt)', () => {
    for (const vals of [[5], [0, 0], [-3, -3, -3]]) {
      const sc = limits(vals)!;
      expect(sc.max - sc.min).toBeGreaterThan(0);
    }
  });
});

describe('isNewer: frame ordering', () => {
  const st = (over: Partial<DeviceState>): DeviceState =>
    ({ boots: 41, uptime_s: 1000, speed: 5, ...over }) as DeviceState;

  it('accepts the first frame', () => {
    expect(isNewer(st({}), null)).toBe(true);
  });

  it('rejects a poll that was issued before the state we already have', () => {
    // The concrete bug: click a speed, its response paints, then a poll issued
    // 300 ms earlier lands and repaints the rail back to the old speed.
    expect(isNewer(st({ uptime_s: 900 }), st({ uptime_s: 1000 }))).toBe(false);
  });

  it('accepts a later frame from the same boot', () => {
    expect(isNewer(st({ uptime_s: 1015 }), st({ uptime_s: 1000 }))).toBe(true);
  });

  it('accepts a frame with equal uptime (same-second responses)', () => {
    expect(isNewer(st({ uptime_s: 1000 }), st({ uptime_s: 1000 }))).toBe(true);
  });

  it('accepts a reboot, whose uptime goes backwards', () => {
    // uptime alone would reject this forever; boots is what breaks the tie.
    expect(isNewer(st({ boots: 42, uptime_s: 3 }), st({ boots: 41, uptime_s: 9000 }))).toBe(true);
  });

  it('rejects a straggler from the PREVIOUS boot', () => {
    expect(isNewer(st({ boots: 41, uptime_s: 9000 }), st({ boots: 42, uptime_s: 3 }))).toBe(false);
  });
});

describe('storage: a nearly-full card has to look nearly full', () => {
  it('stays terse when there is plenty of room', () => {
    expect(storage(1024, 29000, 27976)).toBe('1.00 / 28.3 GB');
  });

  it('names the free space once the card is tight', () => {
    // The deployed card: 210 MB free of 28887, which read as an unremarkable
    // "28.00 / 28.2 GB" and was the reason nobody noticed.
    const s = storage(28677, 28887, 210);
    expect(s).toContain('210 MB free');
    expect(s).toContain('99% full');
  });

  it('switches to GB for larger remainders', () => {
    expect(storage(27000, 29000, 2000)).toContain('2.0 GB free');
  });

  it('omits the detail when the device did not report free space', () => {
    expect(storage(28677, 28887)).toBe('28.00 / 28.2 GB');
  });

  it('cardTight fires at 90% and not before', () => {
    expect(cardTight(8999, 10000)).toBe(false);
    expect(cardTight(9000, 10000)).toBe(true);
    expect(cardTight(0, 0)).toBe(false); // no card: not "tight", just absent
  });
});

describe('axisLabel: one format per range', () => {
  // 2026-08-16 15:27 local, whatever zone the test box is in.
  const t = Math.floor(new Date(2026, 7, 16, 15, 27).getTime() / 1000);

  it('gives 24 h the clock', () => {
    expect(axisLabel(t, 1)).toBe('15:27');
  });

  it('gives a week the date AND the hour', () => {
    // Without the hour two neighbouring ticks both read "8/16" and the axis
    // looks broken.
    expect(axisLabel(t, 7)).toBe('8/16 15h');
  });

  it('drops the hour past a week', () => {
    // 30 and 60 day rows are 2.6 and 5.1 hours apart, so an hour on the label
    // claims a precision the sample does not have -- and it is what made the
    // long axis unreadable at phone width.
    expect(axisLabel(t, 30)).toBe('8/16');
    expect(axisLabel(t, 60)).toBe('8/16');
  });
});

describe('the scrub readouts each say one thing', () => {
  it('distinguishes a stopped fan from an unlogged one', () => {
    // Not the same fact: rows written before the speed column existed have no
    // speed at all, and rendering those as "off" invents a reading.
    expect(speedReadout(7)).toBe('speed 7');
    expect(speedReadout(0)).toBe('off');
    expect(speedReadout(undefined)).toBe('');
  });

  it('names the charger only while it was actually running', () => {
    expect(battReadout(4.187, false)).toBe('4.19 V');
    expect(battReadout(4.187, true)).toBe('4.19 V ⚡ charging');
    expect(battReadout(null, true)).toBe('');
  });
});

describe('powerReadout', () => {
  it('is the watts alone on a steady bucket', () => {
    expect(powerReadout(20.3, 0)).toBe('20.3 W');
    expect(powerReadout(20.3, null)).toBe('20.3 W');
    expect(powerReadout(null, 3)).toBe('');
    // A range narrower than a watt is the meter breathing, not a story.
    expect(powerReadout(20.3, 0, 20.1, 20.5)).toBe('20.3 W');
  });

  it('prints the range when the draw moved inside the bucket', () => {
    // One snapshot per five minutes is what let a cycling fan read as an
    // ordinary number at whatever instant the sample landed on.
    expect(powerReadout(45.1, 0, 4.3, 45.6)).toBe('4.3–45.6 W');
    expect(powerReadout(45.1, 3, 4.3, 45.6)).toBe('4.3–45.6 W · cycling ×3');
  });

  it('names the cycling flips the meter confirmed in the bucket', () => {
    // The scrub readout's half of what the power row's red tint says: the
    // 2026-08-20 night was nine hours of this at a held speed 10.
    expect(powerReadout(45.1, 3)).toBe('45.1 W · cycling ×3');
  });
});

describe('the PLUG status bit', () => {
  const plug = {
    w: 45.1, v: 120.9, age_s: 3, verdict: -1 as const, cycling: true, flips: 5,
    expect_w: 30.2, implied_spd: 12,
  };

  it('says CYCLING with the flip count, and goes red', () => {
    expect(plugBit(plug)).toBe('45.1 W · CYCLING ×5');
    expect(plugColour(plug)).toBe('#e0a9a9');
  });

  it('names the speed the draw implies when the meter disagrees', () => {
    // Not the volts: "45.1 W · looks like speed 12" is the sentence the
    // 2026-08-20 night needed, and nothing said it.
    expect(plugBit({ ...plug, cycling: false })).toBe('45.1 W · looks like speed 12');
  });

  it('reads watts and volts when the fan is steady', () => {
    const ok = {
      ...plug, verdict: 1 as const, cycling: false, flips: 0, w: 20.3, implied_spd: 9,
    };
    expect(plugBit(ok)).toBe('20.3 W · 120.9 V');
    expect(plugColour(ok)).not.toBe('#e0a9a9');
  });

  it('treats no meter as absence, not a fault', () => {
    expect(plugBit(null)).toBe('no meter');
  });
});
