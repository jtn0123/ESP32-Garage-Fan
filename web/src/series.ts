// Turning /api/history into something the charts can draw.
//
// Two jobs: normalise the wire format (Celsius -> Fahrenheit, the -999 outdoor
// sentinel -> null, absent series -> empty), and merge the 24 h window with
// what localStorage remembers so a device reboot does not blank the chart.

import type { History } from './types.js';

export interface Series {
  n: number;
  /** Epoch seconds for sample i, or null before SNTP has synced. */
  ts: (i: number) => number | null;
  night: boolean[];
  tf: (number | null)[]; // garage, Fahrenheit
  of: (number | null)[]; // outside, Fahrenheit
  rh: (number | null)[];
  hpa: (number | null)[];
  spd: number[];
  bv: (number | null)[];
  chg: number[];
}

/** The outdoor column stores -999 rather than null in the CSV path. */
const OUTDOOR_ABSENT = -100;

export function build(h: History): Series {
  const n = h.temp_c?.length ?? 0;
  const ts = (i: number): number | null =>
    h.end_ts ? h.end_ts - (n - 1 - i) * h.interval_s : null;

  const night: boolean[] = [];
  for (let i = 0; i < n; i++) {
    const t = ts(i);
    if (t === null) {
      night.push(false);
      continue;
    }
    const hr = new Date(t * 1000).getHours();
    night.push(hr >= 20 || hr < 6);
  }

  const keep = (a: (number | null)[] | undefined): (number | null)[] => a ?? [];

  return {
    n,
    ts,
    night,
    tf: (h.temp_c ?? []).map((v) => (v === null ? null : v * 9 / 5 + 32)),
    of: (h.out_f ?? []).map((v) => (v === null || v <= OUTDOOR_ABSENT ? null : v)),
    rh: keep(h.rh),
    hpa: keep(h.hpa),
    spd: h.spd ?? [],
    bv: keep(h.batt_v),
    chg: h.chg ?? [],
  };
}

/** Does a series carry at least one real reading? */
export function hasData(series: readonly (number | null)[] | undefined): boolean {
  return !!series && series.some((v) => v !== null && !Number.isNaN(v));
}

interface CachedSample {
  t: number | null;
  r: number | null;
  p: number | null;
  o: number | null;
  s: number;
  b: number | null;
  c: number;
}

const CACHE_KEY = 'gf24';
const DAY_SECONDS = 86_400;

/**
 * Union the freshly fetched 24 h window with the browser's own record of it.
 *
 * The device keeps history in RAM, so a reboot empties the ring and the chart
 * would otherwise restart from one point. Only applies to the 24 h range: the
 * longer ranges come off the SD card and are already complete.
 *
 * Storage is injected so the tests can exercise this without a real
 * localStorage, and so a browser with storage disabled degrades to
 * pass-through rather than throwing on every poll.
 */
export function mergeCache(h: History, store: Storage | null = safeStorage()): History {
  if (!h.end_ts || !store) return h;

  let cache: Record<string, CachedSample> = {};
  try {
    cache = JSON.parse(store.getItem(CACHE_KEY) ?? '{}') as Record<string, CachedSample>;
  } catch {
    cache = {};
  }

  const n = h.temp_c?.length ?? 0;
  for (let i = 0; i < n; i++) {
    const ts = h.end_ts - (n - 1 - i) * h.interval_s;
    cache[String(ts)] = {
      t: h.temp_c[i] ?? null,
      r: h.rh[i] ?? null,
      p: h.hpa[i] ?? null,
      o: h.out_f?.[i] ?? null,
      s: h.spd?.[i] ?? 0,
      b: h.batt_v?.[i] ?? null,
      c: h.chg?.[i] ?? -1,
    };
  }

  const cutoff = h.end_ts - DAY_SECONDS;
  const keys = Object.keys(cache)
    .map(Number)
    .filter((t) => Number.isFinite(t) && t >= cutoff)
    .sort((a, b) => a - b);
  if (!keys.length) return h;

  const out: History = {
    interval_s: h.interval_s,
    end_ts: keys[keys.length - 1]!,
    temp_c: [],
    rh: [],
    hpa: [],
    out_f: [],
    spd: [],
    batt_v: [],
    chg: [],
  };
  const pruned: Record<string, CachedSample> = {};
  for (const t of keys) {
    const c = cache[String(t)]!;
    out.temp_c.push(c.t);
    out.rh.push(c.r);
    out.hpa.push(c.p);
    out.out_f!.push(c.o);
    out.spd!.push(c.s);
    out.batt_v!.push(c.b ?? null);
    out.chg!.push(c.c ?? -1);
    pruned[String(t)] = c;
  }

  try {
    store.setItem(CACHE_KEY, JSON.stringify(pruned));
  } catch {
    // Quota exceeded or storage disabled: the merge still stands for this
    // render, we just will not remember it next time.
  }
  return out;
}

function safeStorage(): Storage | null {
  try {
    return window.localStorage;
  } catch {
    return null; // Safari throws outright when storage is blocked.
  }
}
