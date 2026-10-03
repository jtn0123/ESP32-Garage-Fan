// The chart engine: the drawing primitives, temperature with its differential
// band, restart marks and the shared time axis. The toggleable rows under the
// temperature chart (fan, humidity, pressure, battery, power, gas) draw with
// these primitives from chart_rows.ts.
//
// Hand-rolled canvas rather than a charting library, for the same reason the
// page has no framework: the whole thing ships inside the firmware image.

import { at } from './dom.js';
import { axisLabel } from './format.js';
import type { Series } from './series.js';
import type { BootMark } from './types.js';
import { DIM, OR, OUT, PAD_LEFT as L, PAD_RIGHT as R } from './theme.js';

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

export interface Scale {
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
export const MIN_SPAN = 2;

/**
 * `lowest` is the least value the quantity can physically take. Watts and the
 * gas indices cannot go negative, but the 12 % padding below the data drew
 * their axes down to -1.3 W, -13 and "-0" anyway.
 */
export function limits(
  values: readonly (number | null)[],
  minSpan = MIN_SPAN,
  lowest = -Infinity,
): Scale | null {
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
  return scale(Math.max(mn - pad, lowest), mx + pad);
}

export function scale(min: number, max: number): Scale {
  return { min, max, ticks: [min, (min + max) / 2, max] };
}

// Position by the sample's time fraction, not its index: after a merge with
// holes in it (reboots, outages), equal index spacing would compress the
// missing stretch and every slope around it would lie.
export const xAt = (s: Series, i: number, W: number): number => L + (s.frac[i] ?? 0) * (W - L - R);

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

export const yAt = (v: number, H: number, s: Scale): number =>
  H - 6 - ((v - s.min) * (H - 16)) / (s.max - s.min);

/**
 * Coarsest row spacing the overnight stripes still describe honestly.
 *
 * Night is derived per ROW, so a stripe edge can only land where a row does.
 * At the 24 h and 7 day resolutions (300 s and ~2400 s) that is within an hour
 * of the real 20:00/06:00 boundary. At 30 and 60 days each row covers 2.6 and
 * 5.1 hours, so the same shading would place sunset up to half a night out and
 * render as banding that says nothing. No stripes beats wrong stripes.
 */
const NIGHT_MAX_STEP = 3600;

/** Does the temperature chart shade nights for this series? The legend asks too. */
export const shadesNights = (s: Series): boolean =>
  s.step <= NIGHT_MAX_STEP && s.night.some(Boolean);

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
    // Between the top and bottom gridlines (see yAt), not the whole canvas:
    // full height poked a grey block above the top gridline, where it read as
    // a stray rectangle rather than as part of the plot.
    c.fillRect(x0, 10, xAt(s, j - 1, W) - x0 || 1, H - 16);
    i = j;
  }
}

/**
 * "-0" -> "0", "-0.0°" -> "0.0°": a tick a hair under zero rounds to a signed
 * zero, and an axis reading "-0" looks like a bug even when it is arithmetic.
 */
export const unsignedZero = (label: string): string =>
  /^-0(\.0+)?(?![.\d])/.test(label) ? label.slice(1) : label;

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
export function frame(
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
    c.fillText(unsignedZero(fmt(v)), L - 5, y + 3);
  }
}

export function line(
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

export function crosshair({ c, W, H }: Surface, s: Series, index: number): void {
  if (index < 0) return;
  c.strokeStyle = 'rgba(230,233,237,.5)';
  c.lineWidth = 1;
  const x = xAt(s, index, W) + 0.5;
  c.beginPath();
  c.moveTo(x, 0);
  c.lineTo(x, H - 2);
  c.stroke();
}

export function placeholder({ c, W, H }: Surface, message: string): void {
  c.fillStyle = DIM;
  c.font = '12px "JetBrains Mono",monospace';
  c.textAlign = 'center';
  c.fillText(message, W / 2, H / 2);
}

/**
 * Segment i -> i+1 of the band: true where the garage is the warmer trace,
 * false where the yard is, null where there is nothing to tint -- a missing
 * reading, or s.gap[i+1], a pair straddling an outage, where filling would
 * invent a differential across the hole.
 */
function segmentWarm(s: Series, i: number): boolean | null {
  const a = at(s.tf, i);
  const b = at(s.tf, i + 1);
  const oa = at(s.of, i);
  const ob = at(s.of, i + 1);
  if (a === null || b === null || oa === null || ob === null || s.gap[i + 1]) return null;
  return (a + b) / 2 >= (oa + ob) / 2;
}

/**
 * The band between the two traces, tinted by which one is on top: orange
 * where the garage is hotter than the yard (the fan can help), blue where it
 * is cooler (running the fan would import heat).
 *
 * One polygon per run of same-tint segments. One per segment drew a hairline
 * seam every five minutes, where the anti-aliased edges of neighbouring
 * translucent quads overlapped -- a texture the data does not have.
 */
function fillDifferential({ c, W, H }: Surface, s: Series, sc: Scale): void {
  let i = 0;
  while (i + 1 < s.n) {
    const warm = segmentWarm(s, i);
    if (warm === null) {
      i++;
      continue;
    }
    let end = i + 1; // the run's last point
    while (end + 1 < s.n && segmentWarm(s, end) === warm) end++;
    c.beginPath();
    for (let k = i; k <= end; k++) c.lineTo(xAt(s, k, W), yAt(at(s.tf, k)!, H, sc));
    for (let k = end; k >= i; k--) c.lineTo(xAt(s, k, W), yAt(at(s.of, k)!, H, sc));
    c.closePath();
    c.fillStyle = warm ? 'rgba(232,131,74,.22)' : 'rgba(59,130,246,.14)';
    c.fill();
    i = end;
  }
}

/**
 * One label per restart, or one per CLUSTER of restarts.
 *
 * A deploy session is three or four reboots inside an hour -- 38 to 80 px
 * apart on the 6 h view, 9 to 20 on the 24 h one -- and the label is ~97 px
 * wide, so per-mark labels are guaranteed to scribble over each other at
 * exactly the moment they matter most. Greedy left-to-right: a mark joins the
 * group when its stem lands before the previous label (plus breathing room)
 * has ended. Exported for the unit tests; the maths is the whole feature.
 */
export function clusterMarks(xs: readonly number[], labelW: number, pad = 8): number[][] {
  const groups: number[][] = [];
  for (let i = 0; i < xs.length; i++) {
    const g = groups[groups.length - 1];
    if (g !== undefined && (xs[i] ?? 0) - (xs[g[0] ?? 0] ?? 0) < labelW + pad) g.push(i);
    else groups.push([i]);
  }
  return groups;
}

/**
 * What a group's one label says.
 *
 * The version, when the record carries one, is the headline: "sw_reset" names
 * an OTA and a plain requested restart indistinguishably, and the question a
 * mark actually answers on this fan is "which firmware went on here". Rows
 * from before 1.26.0 have no version and fall back to the cause alone.
 */
export function bootLabel(group: readonly BootMark[]): string {
  const last = group[group.length - 1];
  if (last === undefined) return '';
  const fw = [...group].reverse().find((b) => b.fw)?.fw;
  if (group.length === 1) {
    const cause = last.cause === 'unknown' ? 'restart' : `restart · ${last.cause}`;
    return fw ? `${cause} → ${fw}` : cause;
  }
  return fw ? `${group.length} restarts → ${fw}` : `${group.length} restarts`;
}

/**
 * Restart stems, drawn last so nothing hides them.
 *
 * This is the half the outage band cannot supply: the band says "no data
 * here", the mark says "because it rebooted, and this is how the last life
 * ended". Restarts happen BETWEEN rows, hence xAtTime rather than a row index.
 * Every restart keeps its own stem and flag; only the LABEL is shared when
 * stems sit closer than the words (see clusterMarks).
 */
function drawBootMarks({ c, W, H }: Surface, s: Series, boots: readonly BootMark[]): void {
  const placed: { x: number; b: BootMark }[] = [];
  for (const b of boots) {
    const x = xAtTime(s, b.ts, W);
    if (x !== null) placed.push({ x, b });
  }
  if (placed.length === 0) return;
  placed.sort((a, b) => a.x - b.x);

  c.font = '9px "JetBrains Mono",monospace';
  const widest = Math.max(...placed.map((p) => c.measureText(bootLabel([p.b])).width));
  const groups = clusterMarks(placed.map((p) => p.x), widest);

  for (const p of placed) {
    c.strokeStyle = RESTART_C;
    c.lineWidth = 1.4;
    c.setLineDash([3, 3]);
    c.beginPath();
    c.moveTo(p.x + 0.5, 0);
    c.lineTo(p.x + 0.5, H - 2);
    c.stroke();
    c.setLineDash([]);
    c.fillStyle = RESTART_C;
    c.beginPath();  // a small dropped flag, so the stem reads as an event
    c.moveTo(p.x, 1);
    c.lineTo(p.x + 7, 4.5);
    c.lineTo(p.x, 8);
    c.closePath();
    c.fill();
  }
  c.fillStyle = RESTART_C;
  for (const g of groups) {
    const marks = g.map((i) => placed[i]!);
    const label = bootLabel(marks.map((m) => m.b));
    const first = marks[0]!.x;
    const flip = first + c.measureText(label).width + 9 > W - R;
    c.textAlign = flip ? 'right' : 'left';
    const anchor = flip ? (marks[marks.length - 1]!.x) : first;
    c.fillText(label, anchor + (flip ? -9 : 9), 9);
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
  frame(surf, s, sc, (v) => `${v.toFixed(0)}°`, shadesNights(s));

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
    const label = axisLabel(t, days);
    const x = Math.min(Math.max(xAt(s, i, W), 22), W - 24);
    if (x - lastX < perLabel * 0.6) continue;
    lastX = x;
    c.fillText(label, x, 14);
  }
}
