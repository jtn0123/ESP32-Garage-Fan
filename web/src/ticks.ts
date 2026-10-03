// Where the gridlines go: round values up the side of every row, round clock
// times along the bottom. Pure, so all of it is unit-tested -- an axis is the
// part of a chart people read numbers off, and a wrong one is believed.
//
// Both halves replace rules that labelled whatever value happened to land
// under them. The y labels were the padded min, midpoint and max (0.0 / 11.7 /
// 23.4 W, 3.86 / 4.05 / 4.24 V), so every row needed arithmetic before it said
// anything. The time labels sat every Nth ROW, which put them at 20:46, 23:26,
// 02:06 -- times nobody thinks in, a different set on every refresh.

export interface Scale {
  min: number;
  max: number;
  /** The gridline values, low to high. */
  ticks: number[];
  /** Decimals the labels need: the step's own, so 0/10/20 W rather than 0.0/10.0/20.0. */
  dp: number;
}

/** Strip the binary residue off k * step: 3 * 0.1 is 0.30000000000000004. */
const clean = (v: number): number => Number(v.toPrecision(12));

/** Decimal places a step's multiples need: 5 -> 0, 0.2 -> 1, 0.05 -> 2. */
export function decimals(step: number): number {
  return Math.max(0, -Math.floor(Math.log10(step) + 1e-9));
}

/** The next 1-2-5 step up: 0.2 -> 0.5 -> 1 -> 2 -> 5 -> 10. */
function coarser(step: number): number {
  const p = 10 ** Math.floor(Math.log10(step) + 1e-9);
  const m = Math.round(step / p);
  return clean((m === 1 ? 2 : m === 2 ? 5 : 10) * p);
}

/**
 * The finest step a row's labels may use, from its minimum span.
 *
 * MIN_SPAN (charts.ts) is defined as "just above the tick formatter's
 * resolution", so the resolution can be read back off it rather than declared
 * twice: the largest 1-2-5 step at most half the span. 2 -> 1 (temperature,
 * humidity, gas, watts), 0.5 -> 0.2 (pressure), 0.1 -> 0.05 (battery). A
 * finer step would label a calm 40.3 %RH night 39.5 / 40.0 / 40.5 -- true, but
 * the flattening floor exists precisely so that range is never drawn tall.
 */
export function resolution(minSpan: number): number {
  const half = minSpan / 2;
  const p = 10 ** Math.floor(Math.log10(half) + 1e-9);
  for (const m of [5, 2, 1]) if (m * p <= half * (1 + 1e-9)) return clean(m * p);
  return clean(p);
}

/** How many multiples of `step` lie in [lo, hi]. */
function count(lo: number, hi: number, step: number): number {
  const eps = step * 1e-6;
  return Math.floor((hi + eps) / step) - Math.ceil((lo - eps) / step) + 1;
}

/** Those multiples. Only ever called once count() has said there are a handful. */
function grid(lo: number, hi: number, step: number): number[] {
  const eps = step * 1e-6;
  const out: number[] = [];
  for (let k = Math.ceil((lo - eps) / step); k * step <= hi + eps; k++) out.push(clean(k * step));
  return out;
}

/** Gridlines this close together read comfortably at 10 px... */
const COMFY_PX = 16;
/** ...and this close still read, just tightly. Closer is a smudge. */
export const TIGHT_PX = 12;
/** An end of the range may stretch this far (of the span) to reach a round value. */
const EDGE_SNAP = 0.1;

/**
 * Gridlines at round values, for a range drawn `plotPx` tall.
 *
 * The bounds stay where the caller's padding put them and the ticks are the
 * round values inside: snapping the bounds out to the step wholesale turned
 * 0-21 W into a 0-30 axis, a third of a 66 px row spent on nothing. The one
 * stretch allowed is an end reaching a round value within 10 % of the span --
 * 0.5-21.9 W becomes 0-21.9 and reads 0 / 10 / 20 instead of 10 / 20.
 *
 * The pick is the finest 1-2-5 step giving 3 to `maxTicks` lines (four on a
 * 66 px row, five on a tall chart) at least COMFY_PX apart; failing that,
 * TIGHT_PX apart; failing that, both ends snapped outward to the step (a flat
 * 40.3 %RH night, 39.2-41.4, has room for one tick at the comfortable step).
 */
export function niceScale(
  lo: number,
  hi: number,
  minStep: number,
  plotPx = 50,
  /** Snapping outward never crosses this: watts and gas indices stop at 0. */
  lowest = -Infinity,
): Scale {
  if (!(hi > lo)) return { min: lo, max: lo + minStep, ticks: [lo], dp: decimals(minStep) };
  const span = hi - lo;
  const maxTicks = plotPx >= 120 ? 5 : 4;
  const steps: number[] = [];
  for (let s = minStep; steps.length === 0 || s <= span; s = coarser(s)) steps.push(s);

  // null rather than a 3000-element grid: VOC raw ticks span ~30000 at step 1.
  const make = (min: number, max: number, step: number): Scale | null =>
    count(min, max, step) > maxTicks
      ? null
      : { min, max, ticks: grid(min, max, step), dp: decimals(step) };
  // The nearest multiple at or beyond each end. Cleaned, then clamped so the
  // float noise cleaning removes can never shave the data's own extreme off.
  const below = (step: number): number =>
    Math.max(Math.min(clean(Math.floor(lo / step + 1e-6) * step), lo), lowest);
  const above = (step: number): number =>
    Math.max(clean(Math.ceil(hi / step - 1e-6) * step), hi);
  const fits = (sc: Scale | null, step: number, minPx: number): sc is Scale =>
    sc !== null && sc.ticks.length >= 3 && (step / (sc.max - sc.min)) * plotPx >= minPx;

  for (const minPx of [COMFY_PX, TIGHT_PX]) {
    for (const step of steps) {
      const inside = make(lo, hi, step);
      if (fits(inside, step, minPx)) return inside;
      const b = below(step);
      const a = above(step);
      const reach = EDGE_SNAP * span;
      const near = make(lo - b <= reach ? b : lo, a - hi <= reach ? a : hi, step);
      if (fits(near, step, minPx)) return near;
    }
  }
  for (const step of steps) {
    const out = make(below(step), above(step), step);
    if (fits(out, step, TIGHT_PX)) return out;
  }
  const last = steps[steps.length - 1] ?? minStep;
  return make(lo, hi, last) ?? { min: lo, max: hi, ticks: [], dp: decimals(last) };
}

// ---------------------------------------------------------------- time axis

const MIN = 60;
const HOUR = 3600;
export const DAY = 86400;

/**
 * Spacings the time axis may use, finest first. Sub-day steps divide a day,
 * so the ticks land on the same clock times every day; multi-day steps are
 * counted in local calendar days.
 */
export const TIME_STEPS = [
  5 * MIN, 10 * MIN, 15 * MIN, 30 * MIN,
  HOUR, 2 * HOUR, 3 * HOUR, 6 * HOUR, 12 * HOUR,
  DAY, 2 * DAY, 7 * DAY, 14 * DAY,
] as const;

/**
 * The finest step whose labels sit at least `minGap` px apart.
 *
 * Desktop at 100 px: 6H gets hours, 12H every 2 h, 24H every 3 h, 7D every
 * midnight, 30D and 60D weekly. A phone at 60 px: 2 h, 3 h, 6 h, every other
 * day, weekly, fortnightly. Too wide a span for even the coarsest step still
 * gets that step -- fewer labels, never overlapping ones.
 */
export function timeStep(spanS: number, plotW: number, minGap: number): number {
  for (const step of TIME_STEPS) if ((step / spanS) * plotW >= minGap) return step;
  return 14 * DAY;
}

/**
 * Unlabelled marks between the labels, finest candidate first.
 *
 * Weekly labels on 30D are 250 px apart on a desktop with nothing between
 * them to count days by; a daily mark gives the ruler back without a word of
 * text. Each candidate divides its label step, so a mark never lands where a
 * label should have.
 */
const MINORS: ReadonlyMap<number, readonly number[]> = new Map([
  [10 * MIN, [5 * MIN]],
  [15 * MIN, [5 * MIN]],
  [30 * MIN, [10 * MIN, 15 * MIN]],
  [HOUR, [15 * MIN, 30 * MIN]],
  [2 * HOUR, [30 * MIN, HOUR]],
  [3 * HOUR, [HOUR]],
  [6 * HOUR, [HOUR, 3 * HOUR]],
  [12 * HOUR, [3 * HOUR, 6 * HOUR]],
  [DAY, [6 * HOUR, 12 * HOUR]],
  [2 * DAY, [12 * HOUR, DAY]],
  [7 * DAY, [DAY]],
  [14 * DAY, [DAY, 7 * DAY]],
]);

/** The minor step for a label step, or null when none fits `minGap` px apart. */
export function minorStep(major: number, spanS: number, plotW: number, minGap = 14): number | null {
  for (const s of MINORS.get(major) ?? []) if ((s / spanS) * plotW >= minGap) return s;
  return null;
}

/** A local calendar date as a day count, the same integer all day long. */
const dayNumber = (d: Date): number =>
  Math.round(Date.UTC(d.getFullYear(), d.getMonth(), d.getDate()) / (DAY * 1000));

/** 1970-01-05, day 4, was a Monday: weekly ticks fall on Mondays. */
const MONDAY = 4;

/**
 * The instants in [t0, tn] a `step` axis labels, in epoch seconds.
 *
 * Built on the LOCAL calendar, the zone the labels and the night shading are
 * read in: stepping by a fixed 10800 s would put 3-hourly ticks on 01:00 /
 * 04:00 for the half of the year after a DST change. setHours/setDate carry
 * overflow into the next day in local wall time, which is what does that.
 */
export function timeTicks(t0: number, tn: number, step: number): number[] {
  const out: number[] = [];
  if (!(tn > t0) || !(step > 0)) return out;
  const day = new Date(t0 * 1000);
  day.setHours(0, 0, 0, 0);
  const push = (d: Date): boolean => {
    const t = d.getTime() / 1000;
    if (t > tn) return false;
    // The spring-forward 02:00 does not exist and resolves to 03:00, which
    // the next slot then names again: one tick, not two on the same pixel.
    if (t >= t0 && t !== out[out.length - 1]) out.push(t);
    return true;
  };
  if (step < DAY) {
    const m = step / MIN;
    for (let k = 0; ; k++) {
      const d = new Date(day);
      d.setHours(0, k * m, 0, 0);
      if (!push(d)) break;
    }
    return out;
  }
  const n = Math.round(step / DAY);
  for (let k = 0; ; k++) {
    const d = new Date(day);
    d.setDate(day.getDate() + k);
    if ((((dayNumber(d) - MONDAY) % n) + n) % n !== 0) {
      if (d.getTime() / 1000 > tn) break;
      continue;
    }
    if (!push(d)) break;
  }
  return out;
}
