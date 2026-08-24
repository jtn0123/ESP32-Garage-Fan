// Turning /api/history into something the charts can draw.
//
// One job now: normalise the wire format (Celsius -> Fahrenheit, the -999
// outdoor sentinel -> null, short series padded to length).
//
// The second job this module used to have -- merging the device's 24 h window
// with a localStorage copy so a reboot did not blank the chart -- is gone.
// That cache existed only because /api/history?days=1 was hardwired to the
// device's RAM ring, which empties on every reboot; the firmware now serves
// all three ranges from the SD card, so the card is the record and the browser
// does not need to keep a shadow copy of it. Removing it also removes what
// that cache got wrong: it was keyed on the constant 'gf24' with no device
// identity, so a browser that opened two fans merged one's readings into the
// other's chart.

import type { History } from "./types.js";

/**
 * The last `hours` of a response, older rows dropped.
 *
 * Ranges under a day are a WINDOW on the 24 h payload, not a query of their
 * own. The firmware answers days=1|7|30|60 and nothing else on purpose -- a
 * malformed question gets an error rather than a plausible answer, which is
 * the rule that caught `?range=7d` serving ring data under a card label -- and
 * the 24 h response already holds every row a 6 or 12 hour view needs. Asking
 * for it again with a new parameter would widen that contract to buy nothing.
 *
 * Slices EVERY array on the object, so a series added later cannot be left at
 * full length and silently drawn against a shorter time axis.
 */
export function tail(raw: History, hours: number): History {
  const ts = raw.ts ?? [];
  const n = ts.length;
  if (n === 0) return raw;
  const want = hours * 3600;
  const last = ts[n - 1] ?? 0;
  let from = 0;
  if (last > 0) {
    // Timestamped rows: cut by real time, so an outage inside the window
    // shortens the row count without shortening the span it represents.
    while (from < n - 1) {
      const t = ts[from] ?? 0;
      if (t > 0 && last - t <= want) break;
      from++;
    }
  } else {
    // Before SNTP every ts is 0 and there is no time to cut by; fall back to
    // the nominal cadence. Approximate, and labelled as such by the chart,
    // which already draws index-proportional until the clock syncs.
    const step = raw.interval_s > 0 ? raw.interval_s : 300;
    from = Math.max(0, n - Math.ceil(want / step));
  }
  if (from === 0) return raw;
  const out: Record<string, unknown> = { ...raw };
  for (const [k, v] of Object.entries(raw)) {
    if (Array.isArray(v)) out[k] = v.slice(from);
  }
  return out as unknown as History;
}

export interface Series {
  n: number;
  /** Epoch seconds for sample i, or null before SNTP has synced. */
  ts: (i: number) => number | null;
  /**
   * Seconds between two neighbouring rows of THIS response, measured from the
   * timestamps. Not the device's 300 s sample cadence: a wide range is
   * decimated to fit the chart, so 7D arrives at ~2400 s per row and 60D at
   * ~18000. Everything that reasons about resolution -- gap detection, the
   * axis, the night shading -- has to use this rather than interval_s.
   */
  step: number;
  /**
   * Horizontal position of sample i as a 0..1 fraction, proportional to TIME:
   * an outage occupies its true width on the axis instead of being squeezed
   * to one sample step. Index-proportional before SNTP has synced.
   */
  frac: number[];
  /**
   * True where the step from sample i-1 to i spans more than ~1.5 intervals --
   * the device was dark. Chart lines break here rather than fabricating a
   * bridge across the hole.
   */
  gap: boolean[];
  night: boolean[];
  tf: (number | null)[]; // garage, Fahrenheit
  of: (number | null)[]; // outside, Fahrenheit
  rh: (number | null)[];
  hpa: (number | null)[];
  spd: number[];
  bv: (number | null)[];
  chg: number[];
  w: (number | null)[]; // fan draw at the plug, watts
  vocr: (number | null)[]; // SGP41 raw ticks
  noxr: (number | null)[];
  voc: (number | null)[]; // gas indices; 0 = warming
  nox: (number | null)[];
  /**
   * Plug run/stop flips the meter confirmed inside each bucket; null = no
   * meter or a pre-1.21.0 row. The watts snapshot aliases a cycling fan into
   * jitter; this is the column that says it happened (net/plug_cycle.h).
   */
  flips: (number | null)[];
  /** The bucket's measured draw range -- the band the power row shades. */
  wmin: (number | null)[];
  wmax: (number | null)[];
}

/**
 * The firmware's CSV path stores -999 when no outdoor reading existed; treat
 * anything at or below this threshold as absent rather than matching the
 * sentinel exactly, so a float round-trip cannot sneak it past the filter.
 */
const OUTDOOR_ABSENT_MAX = -100;

/**
 * Pad a series to `n`, so a response whose columns disagree in length cannot
 * put `undefined` into the charts.
 *
 * build() takes its length from temp_c and used to pass every other array
 * through untouched. Any short series then read undefined past its end and
 * plotted as NaN -- which is exactly what the old 7/30-day response did by
 * omitting four series entirely.
 */
function pad<T>(a: readonly T[] | undefined, n: number, fill: T): T[] {
  const out = (a ?? []).slice(0, n) as T[];
  while (out.length < n) out.push(fill);
  return out;
}

/**
 * The real spacing of these rows, from the stamps themselves.
 *
 * interval_s is the device's SAMPLE cadence -- 300 s, always -- and the
 * firmware says so explicitly ("interval_s is now only a nominal hint; ts[]
 * carries the truth"). It is not the spacing of the rows in a response:
 * /api/history reads the whole window off the card and decimates it to fit
 * kGraphMaxPts=288, so 7 days arrive ~2400 s apart and 60 days ~18300 s apart.
 * Measuring the step against 300 s therefore declared EVERY row of a long
 * range to be an outage, which broke every line into invisible one-point
 * segments and shaded the entire chart red -- the "7D/30D/60D show no data"
 * report.
 *
 * The MEDIAN gap, not the mean: a real outage (or a reboot) is one enormous
 * delta among hundreds of regular ones, and the mean would let it inflate the
 * estimate until the next outage stopped registering as one.
 */
function medianStep(
  ts: (i: number) => number | null,
  n: number,
  fallback: number,
): number {
  const deltas: number[] = [];
  for (let i = 1; i < n; i++) {
    const a = ts(i - 1);
    const b = ts(i);
    if (a !== null && b !== null && b > a) deltas.push(b - a);
  }
  if (!deltas.length) return fallback; // unsynced clock: nothing to measure
  deltas.sort((x, y) => x - y);
  return deltas[deltas.length >> 1] ?? fallback;
}

/**
 * Where each row sits along the x axis, and which rows follow an outage.
 *
 * Positions are proportional to real elapsed time, so a 15-minute hole is
 * 15 minutes wide rather than one row wide. When the window carries no usable
 * span -- every stamp unsynced, a single row, or a clock that ran backwards --
 * it falls back to even spacing, which is honest about ordering and makes no
 * claim about duration.
 */
function timeline(
  ts: (i: number) => number | null,
  n: number,
  step: number,
): { frac: number[]; gap: boolean[] } {
  const t0 = ts(0);
  const tn = ts(n - 1);
  const span = t0 !== null && tn !== null ? tn - t0 : 0;
  const even = (i: number): number => (n > 1 ? i / (n - 1) : 0);
  const frac: number[] = [];
  const gap: boolean[] = [];
  for (let i = 0; i < n; i++) {
    const t = ts(i);
    const timed = span > 0 && t0 !== null && t !== null;
    frac.push(timed ? (t - t0) / span : even(i));
    const prev = i > 0 ? ts(i - 1) : null;
    // `step` is already sanitised by the caller: at interval_s <= 0 this
    // comparison would read "any spacing at all" and flag EVERY row, which
    // (since the outage bands landed) would shade the whole chart red.
    gap.push(prev !== null && t !== null && t - prev > step * 1.5);
  }
  return { frac, gap };
}

/** Which rows fall in the overnight band the temperature chart shades. */
function nights(ts: (i: number) => number | null, n: number): boolean[] {
  const out: boolean[] = [];
  for (let i = 0; i < n; i++) {
    const t = ts(i);
    if (t === null) {
      out.push(false);
      continue;
    }
    const hr = new Date(t * 1000).getHours();
    out.push(hr >= 20 || hr < 6);
  }
  return out;
}

export function build(h: History): Series {
  const n = h.temp_c?.length ?? 0;
  // Real per-row epochs from the device. A 0 means SNTP had not synced when
  // that row was taken, which is "unknown", not 1970.
  const stamps = pad<number>(h.ts, n, 0);
  const nominal =
    Number.isFinite(h.interval_s) && h.interval_s > 0 ? h.interval_s : 300;
  // A 0 means SNTP had not synced when that row was taken -- unknown, not 1970.
  const ts = (i: number): number | null => stamps[i] || null;
  const step = medianStep(ts, n, nominal);

  const { frac, gap } = timeline(ts, n, step);
  const night = nights(ts, n);

  return {
    n,
    ts,
    step,
    frac,
    gap,
    night,
    tf: pad<number | null>(h.temp_c, n, null).map((v) =>
      v === null ? null : (v * 9) / 5 + 32,
    ),
    of: pad<number | null>(h.out_f, n, null).map((v) =>
      v === null || v <= OUTDOOR_ABSENT_MAX ? null : v,
    ),
    rh: pad<number | null>(h.rh, n, null),
    hpa: pad<number | null>(h.hpa, n, null),
    spd: pad<number>(h.spd, n, 0),
    bv: pad<number | null>(h.batt_v, n, null),
    chg: pad<number>(h.chg, n, -1),
    w: pad<number | null>(h.watts, n, null),
    vocr: pad<number | null>(h.voc_raw, n, null),
    noxr: pad<number | null>(h.nox_raw, n, null),
    voc: pad<number | null>(h.voc, n, null),
    nox: pad<number | null>(h.nox, n, null),
    flips: pad<number | null>(h.flips, n, null),
    wmin: pad<number | null>(h.w_min, n, null),
    wmax: pad<number | null>(h.w_max, n, null),
  };
}

/** Does a series carry at least one real reading? */
export function hasData(
  series: readonly (number | null)[] | undefined,
): boolean {
  return !!series && series.some((v) => v !== null && !Number.isNaN(v));
}
