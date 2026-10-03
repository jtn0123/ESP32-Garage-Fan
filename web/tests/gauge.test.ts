// The differential gauge's scale and its held-by-a-limit state. The scale is
// numbers someone reads a value off, so where they land and which ones are
// dropped is pinned here rather than eyeballed per screenshot.

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';
import {
  gaugeGeometry,
  gaugePos,
  gaugeStep,
  gaugeTicks,
  heldTag,
  paintGauge,
  pinned,
  scaleLabel,
} from '../src/gauge.js';
import type { DeviceState } from '../src/types.js';

const state = (over: Partial<DeviceState>): DeviceState =>
  ({
    auto: true,
    on_f: 2.5,
    off_f: 1.5,
    floor_on: true,
    floor_f: 64,
    start_on: true,
    start_f: 74,
    limit: null,
    ...over,
  }) as DeviceState;

describe('gaugeGeometry', () => {
  it('keeps the long-standing track for an ordinary band', () => {
    // 0 at 46 %, 6.4 % per degree: every existing install draws as before.
    const g = gaugeGeometry(2.5);
    expect(g.perF).toBe(6.4);
    expect(gaugePos(0, g)).toBe(46);
    expect(gaugePos(2.5, g)).toBeCloseTo(62, 6);
  });

  it('compresses rather than pinning an engage point past the end cap', () => {
    // Settings allows +20. At 6.4 %/°F that sat on the cap under a "+6" label.
    const g = gaugeGeometry(12);
    expect(gaugePos(12, g)).toBeLessThan(97);
    expect(g.hi).toBeGreaterThanOrEqual(12 + 2 - 1e-9);
  });

  it('clamps to the caps and says which one', () => {
    const g = gaugeGeometry(2.5);
    expect(gaugePos(40, g)).toBe(97);
    expect(gaugePos(-40, g)).toBe(2);
    expect(pinned(40, g)).toBe('hi');
    expect(pinned(-40, g)).toBe('lo');
    expect(pinned(1.7, g)).toBeNull();
  });
});

describe('gauge scale', () => {
  it('labels every 2 °F on a desk and on the smallest phone', () => {
    const g = gaugeGeometry(2.5);
    expect(gaugeStep(g, 1050)).toBe(2);
    expect(gaugeStep(g, 288)).toBe(2);
  });

  it('labels the default range -6..+6 with signed numbers', () => {
    const ticks = gaugeTicks(gaugeGeometry(2.5), 1050, []);
    const labels = ticks.filter((t) => t.label !== null).map((t) => t.label);
    expect(labels).toEqual(['−6', '−4', '−2', '0', '+2', '+4', '+6']);
    // Minor ticks between, every degree, inside the track.
    expect(ticks.filter((t) => !t.major).map((t) => t.v)).toEqual([-5, -3, -1, 1, 3, 5, 7]);
  });

  it('takes a coarser step when the band forces a wider range', () => {
    const g = gaugeGeometry(18);
    const step = gaugeStep(g, 380);
    expect(step).toBe(5);
    const labelled = gaugeTicks(g, 380, []).filter((t) => t.label !== null);
    expect(labelled.length).toBeLessThanOrEqual(8);
    for (const t of labelled) expect(Number.isInteger(t.v / 5), `${t.v}`).toBe(true);
  });

  it('drops a number that a threshold tick would strike through', () => {
    // Engage on an even degree: the +2 tick keeps its mark, loses its number.
    const g = gaugeGeometry(2);
    const plus2 = gaugeTicks(g, 288, [1, 2]).find((t) => t.v === 2)!;
    expect(plus2.major).toBe(true);
    expect(plus2.label).toBeNull();
  });

  it('keeps +2 between the default thresholds where there is room', () => {
    // 1050 px: 33 px either side. 288 px: still clear of both ticks.
    for (const w of [1050, 288]) {
      const plus2 = gaugeTicks(gaugeGeometry(2.5), w, [1.5, 2.5]).find((t) => t.v === 2)!;
      expect(plus2.label, `at ${w}px`).toBe('+2');
    }
  });

  it('survives a gauge that has not been laid out', () => {
    // offsetWidth is 0 in jsdom and on a hidden screen.
    expect(gaugeTicks(gaugeGeometry(2.5), 0, [1.5, 2.5]).some((t) => t.label === '0')).toBe(true);
  });

  it('formats with a real minus and an explicit plus', () => {
    expect(scaleLabel(-4)).toBe('−4');
    expect(scaleLabel(0)).toBe('0');
    expect(scaleLabel(6)).toBe('+6');
  });
});

describe('heldTag', () => {
  it('names the low limit when it is in charge', () => {
    expect(heldTag(state({ limit: 'floor', floor_f: 64 }))).toEqual({
      kind: 'floor',
      text: 'HELD BY LOW LIMIT 64°',
    });
  });

  it('names the start point when it is in charge', () => {
    expect(heldTag(state({ limit: 'start', start_f: 78.5 }))?.text).toBe('HELD BY START POINT 78.5°');
  });

  it('says nothing while the differential is in charge', () => {
    expect(heldTag(state({ limit: null }))).toBeNull();
  });

  it('says nothing in manual mode, whatever a stale frame claims', () => {
    expect(heldTag(state({ auto: false, limit: 'floor' }))).toBeNull();
  });
});

describe('paintGauge: the DOM it writes', () => {
  // The real shell, so a renamed id fails here rather than on the device.
  const shell = readFileSync(resolve(import.meta.dirname, '../src/body.html'), 'utf8');
  const mount = (): void => {
    document.body.innerHTML = shell;
  };
  const labels = (): string[] => [...document.querySelectorAll('#gscale b')].map((b) => b.textContent ?? '');

  it('numbers the track and leaves an ordinary state un-held', () => {
    mount();
    paintGauge(state({}), 1.7, true);
    expect(labels()).toEqual(['−6', '−4', '−2', '0', '+2', '+4', '+6']);
    expect(document.getElementById('gauge')!.className).toBe('');
    expect(document.getElementById('gheld')!.classList.contains('hide')).toBe(true);
  });

  it('marks the gauge held, in the limit\'s kind, with the tag', () => {
    mount();
    paintGauge(state({ limit: 'floor', floor_f: 76 }), 1.7, true);
    expect(document.getElementById('gauge')!.className).toBe('held floor');
    const tag = document.getElementById('gheld')!;
    expect(tag.classList.contains('hide')).toBe(false);
    expect(tag.textContent).toContain('HELD BY LOW LIMIT 76°');
  });

  it('drops the held state while a scrub shows a past differential', () => {
    mount();
    paintGauge(state({ limit: 'start' }), 1.7, false);
    expect(document.getElementById('gauge')!.className).toBe('');
    expect(document.getElementById('gheld')!.classList.contains('hide')).toBe(true);
  });

  it('pins the needle with a direction when the reading is off the scale', () => {
    mount();
    paintGauge(state({}), 25, true);
    expect(document.getElementById('gmark')!.className).toBe('pinhi');
    paintGauge(state({}), 1.7, true);
    expect(document.getElementById('gmark')!.className).toBe('');
  });
});
