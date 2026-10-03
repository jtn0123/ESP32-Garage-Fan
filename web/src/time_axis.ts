// The shared time axis under the stack of plots.
//
// Split from charts.ts when round-clock ticks pushed it past the 500-line
// ceiling. The picking -- which instants, which step, which words -- is pure
// and lives in ticks.ts and format.ts; this file only puts it on the canvas.

import { surface, xAtTime } from './charts.js';
import { tickLabel } from './format.js';
import type { Series } from './series.js';
import { minorStep, timeStep, timeTicks } from './ticks.js';
import { DIM, PAD_LEFT as L, PAD_RIGHT as R } from './theme.js';

/** Axis tick marks: the DIM label colour at half strength, minors fainter still. */
const TICK_C = 'rgba(125,135,149,.55)';
const MINOR_C = 'rgba(125,135,149,.28)';

/** A short vertical mark hanging from the top edge of the axis strip. */
function mark(c: CanvasRenderingContext2D, x: number, length: number, colour: string): void {
  c.strokeStyle = colour;
  c.beginPath();
  c.moveTo(Math.round(x) + 0.5, 0);
  c.lineTo(Math.round(x) + 0.5, length);
  c.stroke();
}

/**
 * The time axis: labels on round local clock times (see ticks.ts), each at
 * the x its instant really has. xAtTime is the same time-proportional map the
 * rows are drawn with, so a tick inside an outage lands inside its band.
 *
 * A short mark above each label pins it to its instant, because a label
 * near either edge is nudged inward to stay on the canvas and would otherwise
 * sit a few px off the time it names. Fainter, shorter marks between labels
 * count the hours or days the labels skip.
 */
export function drawAxis(canvas: HTMLCanvasElement, s: Series): void {
  const surf = surface(canvas);
  if (!surf) return;
  const { c, W } = surf;
  if (s.n < 2) return;
  const t0 = s.ts(0);
  const tn = s.ts(s.n - 1);
  // Unsynced ends mean the rows are spaced by index, not time: there is no
  // instant any x stands for, so no clock time can honestly be written under it.
  if (t0 === null || tn === null || tn <= t0) return;
  const plotW = W - L - R;
  // ~100 px between labels on a desktop, ~60 on a phone.
  const step = timeStep(tn - t0, plotW, Math.min(100, Math.max(60, W * 0.09)));
  const majors = timeTicks(t0, tn, step);
  c.lineWidth = 1;
  const minor = minorStep(step, tn - t0, plotW);
  if (minor !== null) {
    const labelled = new Set(majors);
    for (const t of timeTicks(t0, tn, minor)) {
      const x = xAtTime(s, t, W);
      if (x !== null && !labelled.has(t)) mark(c, x, 3, MINOR_C);
    }
  }
  c.font = '10px "JetBrains Mono",monospace';
  c.textAlign = 'center';
  c.fillStyle = DIM;
  for (const t of majors) {
    const x = xAtTime(s, t, W);
    if (x === null) continue;
    mark(c, x, 5, TICK_C);
    const label = tickLabel(t, step);
    const half = c.measureText(label).width / 2;
    c.fillText(label, Math.min(Math.max(x, half + 1), W - half - 1), 16);
  }
}
