import { describe, expect, it } from 'vitest';
import { build, hasData, mergeCache } from '../src/series.js';
import type { History } from '../src/types.js';

const h = (over: Partial<History> = {}): History => ({
  interval_s: 300,
  end_ts: 1_786_071_600,
  temp_c: [20, 21, 22],
  rh: [50, 51, 52],
  hpa: [1010, 1011, 1012],
  out_f: [70, -999, 72],
  spd: [0, 9, 9],
  batt_v: [3.9, null, 4.0],
  chg: [0, 1, 1],
  ...over,
});

class FakeStorage implements Storage {
  private map = new Map<string, string>();
  get length(): number {
    return this.map.size;
  }
  clear(): void {
    this.map.clear();
  }
  getItem(k: string): string | null {
    return this.map.get(k) ?? null;
  }
  key(i: number): string | null {
    return [...this.map.keys()][i] ?? null;
  }
  removeItem(k: string): void {
    this.map.delete(k);
  }
  setItem(k: string, v: string): void {
    this.map.set(k, v);
  }
}

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

  it('tolerates the SD-range shape where optional series are absent', () => {
    // 7-day and 30-day responses carry only temp/rh/hpa (see types.ts).
    const sd: History = {
      interval_s: 1800,
      end_ts: 1_786_071_600,
      temp_c: [20, 21, 22],
      rh: [50, 51, 52],
      hpa: [1010, 1011, 1012],
    };
    const s = build(sd);
    expect(s.n).toBe(3);
    expect(s.of).toEqual([]);
    expect(s.spd).toEqual([]);
  });

  it('returns null timestamps before SNTP has synced', () => {
    const s = build(h({ end_ts: 0 }));
    expect(s.ts(0)).toBeNull();
    expect(s.night.every((v) => v === false)).toBe(true);
  });

  it('spaces timestamps by the interval, newest last', () => {
    const s = build(h());
    expect(s.ts(2)).toBe(1_786_071_600);
    expect(s.ts(0)).toBe(1_786_071_600 - 2 * 300);
  });

  it('spaces uniformly and flags no gaps without explicit timestamps', () => {
    const s = build(h());
    expect(s.frac).toEqual([0, 0.5, 1]);
    expect(s.gap).toEqual([false, false, false]);
  });

  it('positions samples by real time and flags the hole across an outage', () => {
    const base = 1_786_000_000;
    // Three 5-min samples, then the device was dark for two hours.
    const stamps = [base, base + 300, base + 600, base + 7800];
    const gappy = h({
      temp_c: [20, 21, 22, 23],
      rh: [50, 51, 52, 53],
      hpa: [1010, 1011, 1012, 1013],
      out_f: [70, 71, 72, 73],
      spd: [0, 9, 9, 3],
      batt_v: [3.9, 3.9, 3.9, 3.9],
      chg: [0, 0, 0, 0],
      end_ts: base + 7800,
    });
    const s = build(gappy, stamps);
    expect(s.ts(0)).toBe(base);
    expect(s.ts(3)).toBe(base + 7800);
    // The outage occupies its true width: the first three points huddle at
    // the left, the fourth sits at the far edge.
    expect(s.frac[1]).toBeCloseTo(300 / 7800);
    expect(s.frac[2]).toBeCloseTo(600 / 7800);
    expect(s.frac[3]).toBe(1);
    expect(s.gap).toEqual([false, false, false, true]);
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

describe('mergeCache', () => {
  it('unions the fetch with what the cache remembers', () => {
    const store = new FakeStorage();
    // First fetch: three samples ending at T.
    mergeCache(h(), store);
    // Device rebooted: second fetch has ONE sample, 300 s later.
    const later = h({
      end_ts: 1_786_071_900,
      temp_c: [25],
      rh: [40],
      hpa: [1000],
      out_f: [75],
      spd: [3],
      batt_v: [3.8],
      chg: [0],
    });
    const merged = mergeCache(later, store);
    // The reboot did not blank the chart: old rows survive, new row appended.
    expect(merged.h.temp_c.length).toBe(4);
    expect(merged.h.temp_c[3]).toBe(25);
    expect(merged.h.temp_c[0]).toBe(20);
    expect(merged.h.end_ts).toBe(1_786_071_900);
    // And the union's real epochs ride along for the chart's time axis.
    expect(merged.ts).toEqual([
      1_786_071_000, 1_786_071_300, 1_786_071_600, 1_786_071_900,
    ]);
  });

  it('drops rows older than 24 hours', () => {
    const store = new FakeStorage();
    mergeCache(h(), store);
    const dayLater = h({ end_ts: 1_786_071_600 + 86_400 + 600, temp_c: [30], rh: [1], hpa: [1], out_f: [1], spd: [0], batt_v: [null], chg: [0] });
    const merged = mergeCache(dayLater, store);
    expect(merged.h.temp_c).toEqual([30]);
  });

  it('passes through untouched with no storage or no timestamp', () => {
    const noTs = h({ end_ts: 0 });
    expect(mergeCache(noTs, null)).toEqual({ h: noTs, ts: null });
    const fresh = h();
    expect(mergeCache(fresh, null)).toEqual({ h: fresh, ts: null });
  });

  it('survives corrupted cache JSON', () => {
    const store = new FakeStorage();
    store.setItem('gf24', '{not json');
    const merged = mergeCache(h(), store);
    expect(merged.h.temp_c.length).toBe(3);
  });

  it('a reboot hole survives the merge and reaches the gap flags', () => {
    const store = new FakeStorage();
    mergeCache(h(), store); // three samples ending at T
    // The device was dark for two hours, then came back with one sample.
    const afterOutage = h({
      end_ts: 1_786_071_600 + 7200,
      temp_c: [26],
      rh: [45],
      hpa: [1005],
      out_f: [74],
      spd: [6],
      batt_v: [3.85],
      chg: [1],
    });
    const merged = mergeCache(afterOutage, store);
    const s = build(merged.h, merged.ts ?? undefined);
    expect(s.n).toBe(4);
    expect(s.gap).toEqual([false, false, false, true]);
    expect(s.ts(3)).toBe(1_786_071_600 + 7200);
  });
});
