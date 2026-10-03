// The winter limits on the temperature chart: one labelled reference line per
// limit that is switched on, so the garage can be watched approaching it. A
// limit too far from the data to draw to scale becomes a marker on the edge it
// lies beyond instead of flattening the day into a strip.
//
// Built on the charts.ts primitives, like chart_rows.ts; charts.ts knows only
// the generic TempOverlay this hands it.

import { at } from './dom.js';
import { type Scale, type Surface, type TempOverlay, xAt, yAt } from './charts.js';
import { deg } from './reason.js';
import type { Series } from './series.js';
import { LIMIT_COLOURS, PAD_LEFT as L, PAD_RIGHT as R } from './theme.js';
import type { AutoLimit, DeviceState } from './types.js';

export interface LimitLine {
  kind: AutoLimit;
  f: number;
  /** Chart and legend name: "LOW LIMIT" / "START". */
  name: string;
  colour: string;
}

/** The limits switched on, low first. Off is off: a stored value is not a line. */
export function chartLimits(s: DeviceState | null): LimitLine[] {
  if (!s) return [];
  const out: LimitLine[] = [];
  if (s.floor_on) out.push({ kind: 'floor', f: s.floor_f, name: 'LOW LIMIT', colour: LIMIT_COLOURS.floor });
  if (s.start_on) out.push({ kind: 'start', f: s.start_f, name: 'START', colour: LIMIT_COLOURS.start });
  return out;
}

/* ----------------------------------------------------------------- the rule */

/** Always worth growing the range for: a few degrees is the approach itself. */
export const REACH_MIN_F = 3;
/** Past this share of the data's own span, growing would squash the day. */
export const REACH_FRAC = 0.5;

/**
 * How far beyond the data, °F, a limit may sit and still be drawn to scale.
 *
 * Half the data's span (floor 3 °F): taking in a limit that far costs the
 * traces at most a third of their height, which still reads; further than
 * that and a summer chart with a 64° winter limit on it would spend most of
 * its height on empty air. A limit inside the data is always drawn.
 */
export function limitReach(mn: number, mx: number): number {
  return Math.max(REACH_MIN_F, (mx - mn) * REACH_FRAC);
}

export type Placement = 'line' | 'above' | 'below';

/** A line on the chart (the y-range grows to take it), or an edge marker. */
export function placeLimit(f: number, mn: number, mx: number): Placement {
  const reach = limitReach(mn, mx);
  if (f < mn - reach) return 'below';
  if (f > mx + reach) return 'above';
  return 'line';
}

/** Least and greatest plotted value, or null when there is none. */
export function extent(values: readonly (number | null)[]): { mn: number; mx: number } | null {
  let mn = Infinity;
  let mx = -Infinity;
  for (const v of values) {
    if (v === null || Number.isNaN(v)) continue;
    if (v < mn) mn = v;
    if (v > mx) mx = v;
  }
  return mn > mx ? null : { mn, mx };
}

/* ------------------------------------------------------------ label placing */

export interface Box {
  x0: number;
  x1: number;
  y0: number;
  y1: number;
}

/** The vertical span the traces (and the band between them) cover at one x. */
export interface Column {
  x: number;
  y0: number;
  y1: number;
}

/** How many data columns and earlier tags a box would sit on. */
export function collisions(b: Box, cols: readonly Column[], boxes: readonly Box[], pad = 4): number {
  let n = 0;
  for (const c of cols) {
    if (c.x >= b.x0 - pad && c.x <= b.x1 + pad && c.y1 >= b.y0 - pad && c.y0 <= b.y1 + pad) n++;
  }
  for (const o of boxes) {
    if (o.x0 < b.x1 + pad && b.x0 < o.x1 + pad && o.y0 < b.y1 + pad && b.y0 < o.y1 + pad) n += 1000;
  }
  return n;
}

/**
 * The first candidate the data leaves clear, else the least covered. The
 * caller orders the candidates by preference; a tag carries an opaque backing,
 * so a forced overlap still reads -- it just hides a sliver of trace.
 */
export function pickBox(cands: readonly Box[], cols: readonly Column[], boxes: readonly Box[]): Box | null {
  let best: Box | null = null;
  let bestN = Infinity;
  for (const b of cands) {
    const n = collisions(b, cols, boxes);
    if (n === 0) return b;
    if (n < bestN) {
      best = b;
      bestN = n;
    }
  }
  return best;
}

/* ------------------------------------------------------------------ drawing */

const FONT = '600 9px "JetBrains Mono",monospace';
const TAG_H = 15;
const TAG_PAD = 5;
/** Plot bounds, matching yAt: the top gridline sits at 10, the bottom at H-6. */
const TOP = 10;
const bottom = (H: number): number => H - 6;

function occupied(s: Series, sc: Scale, W: number, H: number): Column[] {
  const cols: Column[] = [];
  for (let i = 0; i < s.n; i++) {
    const ys = [at(s.tf, i), at(s.of, i)].filter((v): v is number => v !== null).map((v) => yAt(v, H, sc));
    if (ys.length) cols.push({ x: xAt(s, i, W), y0: Math.min(...ys), y1: Math.max(...ys) });
  }
  return cols;
}

/**
 * Where along the line a tag may sit, as a share of the free width: the old
 * end of the window first (beside the y-axis, where a label reads as part of
 * it), then "now", then the gaps between.
 */
const SPOTS = [0, 1, 0.5, 0.25, 0.75] as const;

/** Every candidate, preferred side first: ys is ordered by the caller. */
function boxesAt(W: number, w: number, ys: readonly [number, number][]): Box[] {
  const out: Box[] = [];
  const free = W - R - 4 - w - (L + 4);
  for (const [y0, y1] of ys) {
    for (const f of SPOTS) {
      const x0 = L + 4 + Math.max(0, free) * f;
      out.push({ x0, x1: x0 + w, y0, y1 });
    }
  }
  return out;
}

function tag(c: CanvasRenderingContext2D, b: Box, text: string, colour: string): void {
  c.fillStyle = 'rgba(11,14,19,.9)';
  c.beginPath();
  c.roundRect(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0, 3);
  c.fill();
  c.fillStyle = colour;
  c.textAlign = 'left';
  c.textBaseline = 'middle';
  c.fillText(text, b.x0 + TAG_PAD, (b.y0 + b.y1) / 2 + 0.5);
  c.textBaseline = 'alphabetic';
}

/**
 * The overlay drawTemperature takes: the in-reach limits widen its y-range,
 * the rules go under the traces, the tags over them.
 */
export function limitOverlay(s: Series, lines: readonly LimitLine[]): TempOverlay | null {
  const ext = extent([...s.tf, ...s.of]);
  if (lines.length === 0 || ext === null) return null;
  const placed = lines.map((line) => ({ line, where: placeLimit(line.f, ext.mn, ext.mx) }));
  return {
    include: placed.filter((p) => p.where === 'line').map((p) => p.line.f),
    under({ c, W, H }: Surface, sc: Scale): void {
      c.lineWidth = 1;
      c.setLineDash([2, 3]);
      for (const { line, where } of placed) {
        if (where !== 'line') continue;
        const y = Math.round(yAt(line.f, H, sc)) + 0.5;
        c.strokeStyle = line.colour;
        c.beginPath();
        c.moveTo(L, y);
        c.lineTo(W - R, y);
        c.stroke();
      }
      c.setLineDash([]);
    },
    over({ c, W, H }: Surface, sc: Scale): void {
      c.font = FONT;
      const cols = occupied(s, sc, W, H);
      const done: Box[] = [];
      for (const { line, where } of placed) {
        const text =
          where === 'line' ? `${line.name} ${deg(line.f)}`
          : `${where === 'above' ? '▴' : '▾'} ${line.name} ${deg(line.f)}`;
        const w = c.measureText(text).width + TAG_PAD * 2;
        let ys: [number, number][];
        if (where === 'line') {
          const y = yAt(line.f, H, sc);
          const above: [number, number] = [y - 2 - TAG_H, y - 2];
          const below: [number, number] = [y + 2, y + 2 + TAG_H];
          // Toward the middle of the plot first, and never off it.
          const sides = y > (TOP + bottom(H)) / 2 ? [above, below] : [below, above];
          ys = sides.filter(([y0, y1]) => y0 >= TOP - 8 && y1 <= bottom(H) + 4);
        } else {
          ys = [where === 'above' ? [TOP + 2, TOP + 2 + TAG_H] : [bottom(H) - 2 - TAG_H, bottom(H) - 2]];
        }
        const box = pickBox(boxesAt(W, w, ys), cols, done);
        if (box === null) continue;
        tag(c, box, text, line.colour);
        done.push(box);
      }
    },
  };
}
