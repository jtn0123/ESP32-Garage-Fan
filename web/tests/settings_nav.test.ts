// The settings jump bar: the scroll math as pure functions, and the bar's DOM
// contract with the group renderer (one tab per group, each pointing at the
// element render() actually gave that group).

import { afterEach, describe, expect, it, vi } from 'vitest';
import { buildGroups, render, type Group, type SettingsDeps } from '../src/settings.js';
import { activeSection, jumpTop, paintJump, revealLeft, sectionId } from '../src/settings_nav.js';
import type { DeviceState } from '../src/types.js';

// The mock device's /api/state, trimmed to what the settings spec reads.
const state = {
  speed: 9, auto: true, auto_max: 9, auto_min: 0, on_f: 2.5, off_f: 1.5,
  floor_on: false, floor_f: 64, start_on: false, start_f: 74, limit: null, outside_f: 73.2,
  fw: '1.14.23', slot: 'ota_0', confirmed: true, sensor: true, boots: 41, prev_death: 'sw_reset',
  sd_q: false, sd_total_mb: 28887, sd_used_mb: 28677, sd_free_mb: 210,
  batt: { v: 4.195, pct: 100, chg: true, eta_h: null, mvh: -6 },
  rssi: -63, uptime_s: 1838, ip: '127.0.0.1',
  gas_on: true, gas_spd: 6, gas_voc: 250, gas_active: false, wh_today: 412.5, cost_kwh: 0.15,
} as unknown as DeviceState;

const noop = (): void => {};
const deps: SettingsDeps = {
  state, info: null, update: null, setConfig: noop, toggleAuto: noop, restart: noop,
  formatCard: noop, purgeCard: noop, recheckUpdate: noop,
};

const group = (title: string): Group => ({ title, blurb: '', rows: [] });

function mount(groups: Group[]): { bar: HTMLElement; host: HTMLElement } {
  document.body.innerHTML = '<div id="settings"><nav id="setjump"></nav><div id="groups"></div></div>';
  const host = document.getElementById('groups')!;
  render(host, groups);
  paintJump(groups);
  return { bar: document.getElementById('setjump')!, host };
}

describe('sectionId', () => {
  it('slugs a title into a stable, prefixed id', () => {
    expect(sectionId('AUTO MODE')).toBe('set-auto-mode');
    expect(sectionId('TEMPERATURE LIMITS')).toBe('set-temperature-limits');
    expect(sectionId(' Gas · boost ')).toBe('set-gas-boost');
  });

  it('gives every group in the real spec its own id', () => {
    // The bar is built from this list, so a duplicate id would send two tabs
    // to the same section and leave the other unreachable.
    const ids = buildGroups(deps).map((g) => sectionId(g.title));
    expect(ids.length).toBeGreaterThanOrEqual(7);
    expect(new Set(ids).size).toBe(ids.length);
    for (const id of ids) expect(id).toMatch(/^set-[a-z0-9]+(-[a-z0-9]+)*$/);
  });
});

describe('activeSection', () => {
  it('is the first section until any top reaches the line', () => {
    expect(activeSection([100, 500, 900], 60, false)).toBe(0);
  });

  it('is the last section whose top has reached the line', () => {
    expect(activeSection([-800, -300, 40, 900], 60, false)).toBe(2);
    expect(activeSection([-800, 60, 900], 60, false)).toBe(1); // touching counts
  });

  it('credits the last section at the end of the page, however low its top', () => {
    // UPDATE is shorter than a screen: its top never climbs to the bar.
    expect(activeSection([-2000, -900, 400], 60, true)).toBe(2);
  });

  it('has nothing to point at without sections', () => {
    expect(activeSection([], 60, true)).toBe(-1);
  });
});

describe('revealLeft', () => {
  it('leaves a tab that is already in view alone', () => {
    expect(revealLeft(0, 380, 100, 80)).toBe(0);
  });

  it('scrolls just far enough to clear the right edge and its fade', () => {
    expect(revealLeft(0, 380, 400, 80, 24)).toBe(400 + 80 + 24 - 380);
  });

  it('scrolls back to a tab hidden off the left edge, never below zero', () => {
    expect(revealLeft(300, 380, 150, 80, 24)).toBe(126);
    expect(revealLeft(300, 380, 10, 80, 24)).toBe(0);
  });
});

describe('jumpTop', () => {
  it('sends the first group to the very top, heading and back link included', () => {
    expect(jumpTop(0, 420, 44)).toBe(0);
  });

  it('tucks a later group\'s top rule behind the bar\'s hairline', () => {
    expect(jumpTop(3, 2000, 44)).toBe(1957);
    expect(jumpTop(3, 2000.4, 44.2)).toBe(1957);
  });

  it('never asks for a negative scroll', () => {
    expect(jumpTop(1, 10, 44)).toBe(0);
  });
});

describe('paintJump', () => {
  afterEach(() => vi.restoreAllMocks());

  it('builds one tab per group, each controlling the element render() made', () => {
    const groups = [group('AUTO MODE'), group('SENSORS'), group('UPDATE')];
    const { bar, host } = mount(groups);
    const tabs = Array.from(bar.querySelectorAll('button'));
    expect(tabs.map((t) => t.textContent)).toEqual(['AUTO MODE', 'SENSORS', 'UPDATE']);
    expect(tabs.map((t) => t.getAttribute('aria-controls'))).toEqual(
      Array.from(host.children, (g) => g.id),
    );
    expect(bar.querySelectorAll('[aria-current]')).toHaveLength(1);
  });

  it('keeps the same tabs across repaints, and rebuilds when the groups change', () => {
    // Settings repaints every state frame; rebuilt tabs would lose focus and
    // reset the strip's sideways scroll once a second.
    const groups = [group('POWER'), group('DEVICE')];
    const { bar, host } = mount(groups);
    const first = bar.firstElementChild;
    render(host, groups);
    paintJump(groups);
    expect(bar.firstElementChild).toBe(first);

    paintJump([group('POWER'), group('DEVICE'), group('NETWORK')]);
    expect(bar.firstElementChild).not.toBe(first);
    expect(bar.children).toHaveLength(3);
  });

  it('lights a tapped tab at once and scrolls without motion when asked not to', () => {
    const scroll = vi.spyOn(window, 'scrollTo').mockImplementation(noop);
    Object.defineProperty(window, 'matchMedia', {
      configurable: true,
      value: (q: string) => ({ matches: q.includes('reduce') }),
    });
    const { bar } = mount([group('NETWORK'), group('POWER'), group('DEVICE')]);
    const tabs = bar.querySelectorAll('button');
    tabs[2]!.click();
    expect(tabs[2]!.getAttribute('aria-current')).toBe('location');
    expect(bar.querySelectorAll('[aria-current]')).toHaveLength(1);
    expect(scroll).toHaveBeenCalledWith(expect.objectContaining({ behavior: 'auto' }));
  });
});
