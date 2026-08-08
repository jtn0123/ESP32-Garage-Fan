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
    expect(merged.temp_c.length).toBe(4);
    expect(merged.temp_c[3]).toBe(25);
    expect(merged.temp_c[0]).toBe(20);
    expect(merged.end_ts).toBe(1_786_071_900);
  });

  it('drops rows older than 24 hours', () => {
    const store = new FakeStorage();
    mergeCache(h(), store);
    const dayLater = h({ end_ts: 1_786_071_600 + 86_400 + 600, temp_c: [30], rh: [1], hpa: [1], out_f: [1], spd: [0], batt_v: [null], chg: [0] });
    const merged = mergeCache(dayLater, store);
    expect(merged.temp_c).toEqual([30]);
  });

  it('passes through untouched with no storage or no timestamp', () => {
    const noTs = h({ end_ts: 0 });
    expect(mergeCache(noTs, null)).toBe(noTs);
    const fresh = h();
    expect(mergeCache(fresh, null)).toBe(fresh);
  });

  it('survives corrupted cache JSON', () => {
    const store = new FakeStorage();
    store.setItem('gf24', '{not json');
    const merged = mergeCache(h(), store);
    expect(merged.temp_c.length).toBe(3);
  });
});
