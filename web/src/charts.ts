// The chart engine: temperature with its differential band, plus the
// toggleable fan / humidity / pressure / battery rows and a shared time axis.
//
// Hand-rolled canvas rather than a charting library, for the same reason the
// page has no framework: the whole thing ships inside the firmware image.

import { at } from './dom.js';
import type { Series } from './series.js';
import type { BootMark } from './types.js';
import { hasData } from './series.js';
import { AC, DIM, OK, OR, OUT, PAD_LEFT as L, PAD_RIGHT as R, PU, RH } from './theme.js';

/** Restart stems and their labels -- the same red family as the outage band. */
const RESTART_C = '#e0a9a9';

export interface Surface {
  c: CanvasRenderingContext2D;
  W: number;
  H: number;
}

/** Size the backing store to the device pixel ratio and hand back a clean context. */
export function surface(canvas: HTMLCanvasElement): Surface | null {
  const w = canvas.offsetWidth;
  const h = canvas.offsetHeight;
  const dpr = window.devicePixelRatio || 1;
  if (!w || !h) return null; // laid out but not yet measured, or display:none
  const bw = Math.round(w * dpr);
  const bh = Math.round(h * dpr);
  if (canvas.width !== bw || canvas.height !== bh) {
    canvas.width = bw;
    canvas.height = bh;
  }
  const c = canvas.getContext('2d');
  if (!c) return null;
  c.setTransform(dpr, 0, 0, dpr, 0, 0);
  c.clearRect(0, 0, w, h);
  return { c, W: w, H: h };
}

interface Scale {
  min: number;
  max: number;
  ticks: number[];
}

/**
 * Smallest y-range a chart may auto-scale to.
 *
 * Without a floor, a calm night reading 40.1-40.5 %RH filled the full chart
 * height with dramatic-looking oscillation while all three gridline labels
 * rendered as "40" -- the axis said the swing was nothing and the line said it
 * was everything, and the line is what gets read. The floor is set just above
 * the tick formatters' resolution (0-dp for temperature and humidity) so a
 * range that cannot be distinguished in the labels cannot be exaggerated in
 * the plot either.
 */
const MIN_SPAN = 2;

export function limits(values: readonly (number | null)[], minSpan = MIN_SPAN): Scale | null {
  let mn = Infinity;
  let mx = -Infinity;
  for (const v of values) {
    if (v === null || v === undefined || Number.isNaN(v)) continue;
    if (v < mn) mn = v;
    if (v > mx) mx = v;
  }
  if (mn > mx) return null;
  if (mx - mn < minSpan) {
    // Grow symmetrically about the data so a flat series sits centred rather
    // than pinned to an edge.
    const mid = (mn + mx) / 2;
    mn = mid - minSpan / 2;
    mx = mid + minSpan / 2;
  }
  const pad = (mx - mn) * 0.12;
  return scale(mn - pad, mx + pad);
}

function scale(min: number, max: number): Scale {
  return { min, max, ticks: [min, (min + max) / 2, max] };
}

// Position by the sample's time fraction, not its index: after a merge with
// holes in it (reboots, outages), equal index spacing would compress the
// missing stretch and every slope around it would lie.
const xAt = (s: Series, i: number, W: number): number => L + (s.frac[i] ?? 0) * (W - L - R);

/**
 * A wall-clock instant's x position. Restarts happen INSIDE an outage, i.e.
 * between two rows, so they cannot be placed by row index like everything
 * else. Returns null when the instant is outside the plotted window.
 */
export function xAtTime(s: Series, t: number, W: number): number | null {
  const t0 = s.ts(0);
  const tn = s.ts(s.n - 1);
  if (t0 === null || tn === null || tn <= t0) return null;
  if (t < t0 || t > tn) return null;
  return L + ((t - t0) / (tn - t0)) * (W - L - R);
}

const yAt = (v: number, H: number, s: Scale): number =>
  H - 6 - ((v - s.min) * (H - 16)) / (s.max - s.min);

/** The dim overnight stripes behind the temperature trace. */
function shadeNights({ c, W, H }: Surface, s: Series): void {
  c.fillStyle = 'rgba(255,255,255,.035)';
  let i = 0;
  while (i < s.n) {
    if (!s.night[i]) {
      i++;
      continue;
    }
    let j = i;
    while (j < s.n && s.night[j]) j++;
    const x0 = xAt(s, i, W);
    c.fillRect(x0, 0, xAt(s, j - 1, W) - x0 || 1, H - 2);
    i = j;
  }
}

/**
 * The span where no samples exist, shaded and ruled at both edges.
 *
 * Drawn for EVERY row that frames itself, so a hole in the fan row and a
 * hole in the temperature row read as the same event rather than as two
 * coincidences. s.gap[i] means row i's predecessor is more than 1.5 nominal
 * intervals behind -- the lines already break there; this says why.
 */
function shadeOutages({ c, W, H }: Surface, s: Series): void {
  for (let i = 1; i < s.n; i++) {
    if (!s.gap[i]) continue;
    const x0 = xAt(s, i - 1, W);
    const x1 = xAt(s, i, W);
    c.fillStyle = 'rgba(224,169,169,.10)';
    c.fillRect(x0, 0, Math.max(x1 - x0, 1.5), H - 2);
    c.strokeStyle = 'rgba(224,169,169,.45)';
    c.lineWidth = 1;
    for (const x of [x0, x1]) {
      c.beginPath();
      c.moveTo(x + 0.5, 0);
      c.lineTo(x + 0.5, H - 2);
      c.stroke();
    }
  }
}

/** Gridlines, y-axis labels, and the shading behind them. */
function frame(
  surf: Surface,
  s: Series,
  sc: Scale,
  fmt: (v: number) => string,
  shadeNight: boolean,
): void {
  const { c, W, H } = surf;
  if (shadeNight) shadeNights(surf, s);
  shadeOutages(surf, s);
  c.strokeStyle = '#161c24';
  c.lineWidth = 1;
  c.fillStyle = DIM;
  c.font = '10px "JetBrains Mono",monospace';
  c.textAlign = 'right';
  for (const v of sc.ticks) {
    const y = yAt(v, H, sc);
    c.beginPath();
    c.moveTo(L, y);
    c.lineTo(W - R, y);
    c.stroke();
    c.fillText(fmt(v), L - 5, y + 3);
  }
}

function line(
  { c, W, H }: Surface,
  s: Series,
  sc: Scale,
  values: readonly (number | null)[],
  colour: string,
  dashed: boolean,
  width: number,
): void {
  c.strokeStyle = colour;
  c.lineWidth = width;
  c.lineJoin = 'round';
  if (dashed) c.setLineDash([5, 4]);
  c.beginPath();
  let drawing = false;
  for (let i = 0; i < s.n; i++) {
    const v = at(values, i);
    if (v === null) {
      drawing = false;
      continue;
    }
    if (s.gap[i]) drawing = false; // outage: leave a hole, not a bridge
    const x = xAt(s, i, W);
    const y = yAt(v, H, sc);
    if (drawing) c.lineTo(x, y);
    else c.moveTo(x, y);
    drawing = true;
  }
  c.stroke();
  c.setLineDash([]);
}

function crosshair({ c, W, H }: Surface, s: Series, index: number): void {
  if (index < 0) return;
  c.strokeStyle = 'rgba(230,233,237,.5)';
  c.lineWidth = 1;
  const x = xAt(s, index, W) + 0.5;
  c.beginPath();
  c.moveTo(x, 0);
  c.lineTo(x, H - 2);
  c.stroke();
}

function placeholder({ c, W, H }: Surface, message: string): void {
  c.fillStyle = DIM;
  c.font = '12px "JetBrains Mono",monospace';
  c.textAlign = 'center';
  c.fillText(message, W / 2, H / 2);
}

/**
 * The band between the two traces, tinted by which one is on top: orange
 * where the garage is hotter than the yard (the fan can help), blue where it
 * is cooler (running the fan would import heat).
 */
function fillDifferential({ c, W, H }: Surface, s: Series, sc: Scale): void {
  for (let i = 0; i + 1 < s.n; i++) {
    const a = at(s.tf, i);
    const b = at(s.tf, i + 1);
    const oa = at(s.of, i);
    const ob = at(s.of, i + 1);
    // s.gap[i+1]: the pair straddles an outage, so there is nothing between
    // them to tint -- filling it would invent a differential across the hole.
    if (a === null || b === null || oa === null || ob === null || s.gap[i + 1]) continue;
    c.beginPath();
    c.moveTo(xAt(s, i, W), yAt(a, H, sc));
    c.lineTo(xAt(s, i + 1, W), yAt(b, H, sc));
    c.lineTo(xAt(s, i + 1, W), yAt(ob, H, sc));
    c.lineTo(xAt(s, i, W), yAt(oa, H, sc));
    c.closePath();
    c.fillStyle = (a + b) / 2 >= (oa + ob) / 2 ? 'rgba(232,131,74,.22)' : 'rgba(59,130,246,.14)';
    c.fill();
  }
}

/**
 * Restart stems, drawn last so nothing hides them.
 *
 * This is the half the outage band cannot supply: the band says "no data
 * here", the mark says "because it rebooted, and this is how the last life
 * ended". Restarts happen BETWEEN rows, hence xAtTime rather than a row index.
 */
function drawBootMarks({ c, W, H }: Surface, s: Series, boots: readonly BootMark[]): void {
  for (const b of boots) {
    const x = xAtTime(s, b.ts, W);
    if (x === null) continue;
    const flip = x > W - 70;  // near the right edge, label leftwards
    c.strokeStyle = RESTART_C;
    c.lineWidth = 1.4;
    c.setLineDash([3, 3]);
    c.beginPath();
    c.moveTo(x + 0.5, 0);
    c.lineTo(x + 0.5, H - 2);
    c.stroke();
    c.setLineDash([]);
    c.fillStyle = RESTART_C;
    c.beginPath();  // a small dropped flag, so the stem reads as an event
    c.moveTo(x, 1);
    c.lineTo(x + 7, 4.5);
    c.lineTo(x, 8);
    c.closePath();
    c.fill();
    c.font = '9px "JetBrains Mono",monospace';
    c.textAlign = flip ? 'right' : 'left';
    c.fillText(b.cause === 'unknown' ? 'restart' : `restart · ${b.cause}`, x + (flip ? -9 : 9), 9);
  }
}

export function drawTemperature(
  canvas: HTMLCanvasElement,
  s: Series,
  index: number,
  /** Restart marks inside this window; drawn last so nothing hides them. */
  boots: readonly BootMark[] = [],
): void {
  const surf = surface(canvas);
  if (!surf) return;
  const { c, W, H } = surf;
  if (s.n < 2) {
    placeholder(surf, 'waiting for data — one sample every 5 minutes');
    return;
  }
  const sc = limits([...s.tf, ...s.of]);
  if (!sc) {
    placeholder(surf, 'no data');
    return;
  }
  frame(surf, s, sc, (v) => `${v.toFixed(0)}°`, true);

  fillDifferential(surf, s, sc);

  line(surf, s, sc, s.of, OUT, true, 2);
  line(surf, s, sc, s.tf, OR, false, 2.2);

  drawBootMarks(surf, s, boots);

  if (index >= 0) {
    crosshair(surf, s, index);
    for (const [value, colour] of [
      [at(s.tf, index), OR],
      [at(s.of, index), OUT],
    ] as const) {
      if (value === null) continue;
      const x = xAt(s, index, W);
      const y = yAt(value, H, sc);
      c.fillStyle = '#0b0e13';
      c.beginPath();
      c.arc(x, y, 5, 0, Math.PI * 2);
      c.fill();
      c.fillStyle = colour;
      c.beginPath();
      c.arc(x, y, 3.5, 0, Math.PI * 2);
      c.fill();
    }
  }
}

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

export function drawSimple(
  canvas: HTMLCanvasElement,
  s: Series,
  values: readonly (number | null)[],
  colour: string,
  fmt: (v: number) => string,
  emptyMessage: string,
  index: number,
  /** Smallest range to auto-scale to, in this series' own units. */
  minSpan = MIN_SPAN,
): void {
  const surf = surface(canvas);
  if (!surf) return;
  if (!hasData(values)) {
    placeholder(surf, emptyMessage);
    return;
  }
  const sc = limits(values, minSpan);
  if (!sc) {
    placeholder(surf, emptyMessage);
    return;
  }
  frame(surf, s, sc, fmt, false);
  line(surf, s, sc, values, colour, false, 2);
  crosshair(surf, s, index);
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

export function drawAxis(canvas: HTMLCanvasElement, s: Series, days: number): void {
  const surf = surface(canvas);
  if (!surf) return;
  const { c, W } = surf;
  if (s.n < 2) return;
  c.fillStyle = DIM;
  c.font = '10px "JetBrains Mono",monospace';
  c.textAlign = 'center';
  // Label density follows the available width: ~120px apart on a desktop,
  // tighter on a phone, never fewer than three.
  const perLabel = W < 520 ? 70 : 120;
  const step = Math.max(1, Math.ceil(s.n / Math.max(3, Math.floor((W - L - R) / perLabel))));
  // Time-proportional x means index steps can land labels unevenly around a
  // gap; the lastX guard drops any label that would crowd its neighbour.
  let lastX = -Infinity;
  for (let i = 0; i < s.n; i += step) {
    const t = s.ts(i);
    if (t === null) continue;
    const d = new Date(t * 1000);
    // Multi-day ranges carry the hour too: without it two ticks can both read
    // "8/3" and the axis looks broken.
    const label =
      days === 1
        ? `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`
        : `${d.getMonth() + 1}/${d.getDate()} ${d.getHours()}h`;
    const x = Math.min(Math.max(xAt(s, i, W), 22), W - 24);
    if (x - lastX < perLabel * 0.6) continue;
    lastX = x;
    c.fillText(label, x, 14);
  }
}


export const SERIES_COLOURS = {
  fan: AC,
  humidity: RH,
  pressure: OK,
  battery: PU,
  power: '#e8834a',
  voc: '#22a06b',
  nox: '#b98add',
} as const;
