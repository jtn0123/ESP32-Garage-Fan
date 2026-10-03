// The toggleable rows under the temperature chart: fan speed, the single-line
// sensor rows (humidity, pressure, gas), plug power and battery.
//
// Split from charts.ts at the 500-line ceiling, along the seam the screen
// already has: charts.ts owns the primitives and the temperature chart they
// were built for, and every row here is drawn with those primitives.

import {
  MIN_SPAN,
  crosshair,
  frame,
  limits,
  line,
  placeholder,
  scale,
  surface,
  xAt,
  yAt,
} from './charts.js';
import type { Scale, Surface } from './charts.js';
import { at } from './dom.js';
import type { Series } from './series.js';
import { hasData } from './series.js';
import { AC, PU, SERIES_COLOURS } from './theme.js';

export function drawFanSpeed(canvas: HTMLCanvasElement, s: Series, index: number): void {
  const surf = surface(canvas);
  if (!surf) return;
  const { c, W, H } = surf;
  if (!s.spd.length) {
    placeholder(surf, 'fan history is only kept for the last 24 hours');
    return;
  }
  const sc = scale(0, 12);
  frame(surf, s, sc, (v) => v.toFixed(0), false);

  // Step plot, not a line: the speed holds between samples rather than
  // ramping. One polygon per contiguous run, so an outage leaves a hole
  // instead of fabricating a plateau across the time the device was dark.
  const speed = (i: number): number => s.spd[i] ?? 0;
  c.fillStyle = 'rgba(59,130,246,.28)';
  c.strokeStyle = AC;
  c.lineWidth = 1.6;
  let a = 0;
  for (let i = 1; i <= s.n; i++) {
    if (i < s.n && !s.gap[i]) continue;
    const b = i - 1;
    c.beginPath();
    c.moveTo(xAt(s, a, W), yAt(0, H, sc));
    for (let k = a; k <= b; k++) {
      c.lineTo(xAt(s, k, W), yAt(speed(k), H, sc));
      if (k < b) c.lineTo(xAt(s, k + 1, W), yAt(speed(k), H, sc));
    }
    c.lineTo(xAt(s, b, W), yAt(0, H, sc));
    c.closePath();
    c.fill();

    c.beginPath();
    let prev = speed(a);
    c.moveTo(xAt(s, a, W), yAt(prev, H, sc));
    for (let k = a + 1; k <= b; k++) {
      c.lineTo(xAt(s, k, W), yAt(prev, H, sc));
      c.lineTo(xAt(s, k, W), yAt(speed(k), H, sc));
      prev = speed(k);
    }
    c.stroke();
    a = i;
  }
  crosshair(surf, s, index);
}

export interface RowScale {
  /** Smallest range to auto-scale to, in this series' own units. */
  minSpan?: number;
  /** Least value the quantity can take; the axis never pads below it. */
  lowest?: number;
  /** Axis label for a value; whole numbers unless the row needs finer. */
  fmt?: (v: number) => string;
}

const wholeNumber = (v: number): string => v.toFixed(0);

export function drawSimple(
  canvas: HTMLCanvasElement,
  s: Series,
  values: readonly (number | null)[],
  colour: string,
  emptyMessage: string,
  index: number,
  { minSpan = MIN_SPAN, lowest = -Infinity, fmt = wholeNumber }: RowScale = {},
): void {
  const surf = surface(canvas);
  if (!surf) return;
  if (!hasData(values)) {
    placeholder(surf, emptyMessage);
    return;
  }
  const sc = limits(values, minSpan, lowest);
  if (!sc) {
    placeholder(surf, emptyMessage);
    return;
  }
  frame(surf, s, sc, fmt, false);
  line(surf, s, sc, values, colour, false, 2);
  crosshair(surf, s, index);
}

/**
 * The plug watts, with the buckets the meter saw the fan cycling in tinted.
 *
 * The watts column is one snapshot per five minutes. On 2026-08-20 the fan
 * stopped and restarted every minute or two all night at a held speed 10, and
 * that column drew it as jitter between 4 and 45 W -- technically visible,
 * readable as nothing. `flips` is the firmware's own count of confirmed
 * run/stop edges per bucket; any bucket with one gets the outage-red tint
 * under the line, so a cycling night reads as a red block, not noise.
 */
export function drawPower(canvas: HTMLCanvasElement, s: Series, index: number): void {
  const surf = surface(canvas);
  if (!surf) return;
  if (!hasData(s.w)) {
    placeholder(surf, 'no plug data yet');
    return;
  }
  // Scale to the BAND, not just the snapshot line: a bucket whose meter
  // swung 4->45 W has to fit on the axis or the range it describes is a lie.
  const sc = limits([...s.w, ...s.wmin, ...s.wmax], MIN_SPAN, 0);
  if (!sc) {
    placeholder(surf, 'no plug data yet');
    return;
  }
  frame(surf, s, sc, (v) => v.toFixed(1), false);
  fillMeterBand(surf, s, sc);
  tintCycling(surf, s);
  line(surf, s, sc, s.w, SERIES_COLOURS.power, false, 2);
  crosshair(surf, s, index);
}

/**
 * The min-max range the meter saw inside each 5-minute bucket. This is the
 * half the snapshot line cannot carry: on 2026-08-20 the fan alternated
 * between stopped and flat out inside every bucket, and one sample per
 * bucket drew that as a jittery line at whatever instant it landed on.
 */
function fillMeterBand({ c, W, H }: Surface, s: Series, sc: Scale): void {
  c.fillStyle = 'rgba(216,194,40,.18)'; // SERIES_COLOURS.power, translucent
  let from = 0;
  while (from < s.n) {
    const lo = at(s.wmin, from);
    const hi = at(s.wmax, from);
    if (lo === null || hi === null) {
      from++;
      continue;
    }
    let to = from;
    while (to + 1 < s.n && at(s.wmin, to + 1) !== null && at(s.wmax, to + 1) !== null &&
           !s.gap[to + 1])
      to++;
    c.beginPath();
    for (let i = from; i <= to; i++) c.lineTo(xAt(s, i, W), yAt(at(s.wmax, i)!, H, sc));
    for (let i = to; i >= from; i--) c.lineTo(xAt(s, i, W), yAt(at(s.wmin, i)!, H, sc));
    c.closePath();
    c.fill();
    from = to + 1;
  }
}

/** The outage-red tint under every run of buckets the meter saw cycling. */
function tintCycling({ c, W, H }: Surface, s: Series): void {
  c.fillStyle = 'rgba(224,169,169,.22)';
  let i = 0;
  while (i < s.n) {
    const f = at(s.flips, i);
    if (f !== null && f > 0) {
      let j = i;
      // A run ends at an outage gap, the same rule as the battery tint: the
      // shading must not claim the fan cycled while the device was dark.
      while (j < s.n && (at(s.flips, j) ?? 0) > 0 && (j === i || !s.gap[j])) j++;
      const x0 = xAt(s, i, W);
      c.fillRect(x0, 0, xAt(s, j - 1, W) - x0 || 1.5, H - 2);
      i = j;
    } else {
      i++;
    }
  }
}

export function drawBattery(canvas: HTMLCanvasElement, s: Series, index: number): void {
  const surf = surface(canvas);
  if (!surf) return;
  const { c, W, H } = surf;
  if (!hasData(s.bv)) {
    placeholder(surf, 'battery history is only kept for the last 24 hours');
    return;
  }
  // 0.1 V, not the 2-unit default: a LiPo's ENTIRE working range is about
  // 0.7 V, so the temperature/humidity floor would flatten every real
  // discharge curve into a straight line.
  const sc = limits(s.bv, 0.1);
  if (!sc) {
    placeholder(surf, 'no battery data');
    return;
  }
  // Shade the stretches the charger was active, so a rising line reads as
  // "charging" rather than "mystery".
  c.fillStyle = 'rgba(59,130,246,.14)';
  let i = 0;
  while (i < s.n) {
    if (s.chg[i] === 1) {
      let j = i;
      // A shading run ends at an outage gap: whether the charger ran while
      // the device was dark is unknown, so the tint must not claim it did.
      // (Night shading deliberately differs -- night is clock-derived and
      // true regardless of whether the device was awake to record it.)
      while (j < s.n && s.chg[j] === 1 && (j === i || !s.gap[j])) j++;
      const x0 = xAt(s, i, W);
      c.fillRect(x0, 0, xAt(s, j - 1, W) - x0 || 1, H - 2);
      i = j;
    } else {
      i++;
    }
  }
  frame(surf, s, sc, (v) => v.toFixed(2), false);
  line(surf, s, sc, s.bv, PU, false, 2);
  crosshair(surf, s, index);
}
