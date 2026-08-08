// The chart engine: temperature with its differential band, plus the
// toggleable fan / humidity / pressure / battery rows and a shared time axis.
//
// Hand-rolled canvas rather than a charting library, for the same reason the
// page has no framework: the whole thing ships inside the firmware image.

import { at } from './dom.js';
import type { Series } from './series.js';
import { hasData } from './series.js';
import { AC, DIM, OK, OR, OUT, PAD_LEFT as L, PAD_RIGHT as R, PU, RH } from './theme.js';

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

function limits(values: readonly (number | null)[]): Scale | null {
  let mn = Infinity;
  let mx = -Infinity;
  for (const v of values) {
    if (v === null || v === undefined || Number.isNaN(v)) continue;
    if (v < mn) mn = v;
    if (v > mx) mx = v;
  }
  if (mn > mx) return null;
  if (mn === mx) {
    mn -= 1;
    mx += 1;
  }
  const pad = (mx - mn) * 0.12;
  return scale(mn - pad, mx + pad);
}

function scale(min: number, max: number): Scale {
  return { min, max, ticks: [min, (min + max) / 2, max] };
}

const xAt = (i: number, W: number, n: number): number =>
  L + (i * (W - L - R)) / Math.max(n - 1, 1);

const yAt = (v: number, H: number, s: Scale): number =>
  H - 6 - ((v - s.min) * (H - 16)) / (s.max - s.min);

/** Gridlines, y-axis labels, and the overnight shading on the temperature chart. */
function frame(
  { c, W, H }: Surface,
  s: Series,
  sc: Scale,
  fmt: (v: number) => string,
  shadeNight: boolean,
): void {
  if (shadeNight) {
    c.fillStyle = 'rgba(255,255,255,.035)';
    let i = 0;
    while (i < s.n) {
      if (s.night[i]) {
        let j = i;
        while (j < s.n && s.night[j]) j++;
        const x0 = xAt(i, W, s.n);
        c.fillRect(x0, 0, xAt(j - 1, W, s.n) - x0 || 1, H - 2);
        i = j;
      } else {
        i++;
      }
    }
  }
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
    const x = xAt(i, W, s.n);
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
  const x = xAt(index, W, s.n) + 0.5;
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

export function drawTemperature(canvas: HTMLCanvasElement, s: Series, index: number): void {
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

  // The band between the two traces, tinted by which one is on top: orange
  // where the garage is hotter than the yard (the fan can help), blue where it
  // is cooler (running the fan would import heat).
  for (let i = 0; i + 1 < s.n; i++) {
    const a = at(s.tf, i);
    const b = at(s.tf, i + 1);
    const oa = at(s.of, i);
    const ob = at(s.of, i + 1);
    if (a === null || b === null || oa === null || ob === null) continue;
    c.beginPath();
    c.moveTo(xAt(i, W, s.n), yAt(a, H, sc));
    c.lineTo(xAt(i + 1, W, s.n), yAt(b, H, sc));
    c.lineTo(xAt(i + 1, W, s.n), yAt(ob, H, sc));
    c.lineTo(xAt(i, W, s.n), yAt(oa, H, sc));
    c.closePath();
    c.fillStyle = (a + b) / 2 >= (oa + ob) / 2 ? 'rgba(232,131,74,.22)' : 'rgba(59,130,246,.14)';
    c.fill();
  }

  line(surf, s, sc, s.of, OUT, true, 2);
  line(surf, s, sc, s.tf, OR, false, 2.2);

  if (index >= 0) {
    crosshair(surf, s, index);
    for (const [value, colour] of [
      [at(s.tf, index), OR],
      [at(s.of, index), OUT],
    ] as const) {
      if (value === null) continue;
      const x = xAt(index, W, s.n);
      const y = yAt(value, H, sc);
      c.fillStyle = '#0b0e13';
      c.beginPath();
      c.arc(x, y, 5, 0, 7);
      c.fill();
      c.fillStyle = colour;
      c.beginPath();
      c.arc(x, y, 3.5, 0, 7);
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

  // Step plot, not a line: the speed holds between samples rather than ramping.
  const speed = (i: number): number => s.spd[i] ?? 0;
  c.fillStyle = 'rgba(59,130,246,.28)';
  c.strokeStyle = AC;
  c.lineWidth = 1.6;
  c.beginPath();
  c.moveTo(xAt(0, W, s.n), yAt(0, H, sc));
  for (let i = 0; i < s.n; i++) {
    c.lineTo(xAt(i, W, s.n), yAt(speed(i), H, sc));
    if (i + 1 < s.n) c.lineTo(xAt(i + 1, W, s.n), yAt(speed(i), H, sc));
  }
  c.lineTo(xAt(s.n - 1, W, s.n), yAt(0, H, sc));
  c.closePath();
  c.fill();

  c.beginPath();
  let prev = speed(0);
  c.moveTo(xAt(0, W, s.n), yAt(prev, H, sc));
  for (let i = 1; i < s.n; i++) {
    c.lineTo(xAt(i, W, s.n), yAt(prev, H, sc));
    c.lineTo(xAt(i, W, s.n), yAt(speed(i), H, sc));
    prev = speed(i);
  }
  c.stroke();
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
): void {
  const surf = surface(canvas);
  if (!surf) return;
  if (!hasData(values)) {
    placeholder(surf, emptyMessage);
    return;
  }
  const sc = limits(values);
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
  const sc = limits(s.bv);
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
      while (j < s.n && s.chg[j] === 1) j++;
      const x0 = xAt(i, W, s.n);
      c.fillRect(x0, 0, xAt(j - 1, W, s.n) - x0 || 1, H - 2);
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
    c.fillText(label, Math.min(Math.max(xAt(i, W, s.n), 22), W - 24), 14);
  }
}

export const SERIES_COLOURS = { fan: AC, humidity: RH, pressure: OK, battery: PU } as const;
