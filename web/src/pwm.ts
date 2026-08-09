// The gate-drive waveform: a small preview in the hover card, and the full
// scope panel with its phosphor persistence and refresh sweep.
//
// Both draw the same thing the RMT peripheral is actually emitting on GPIO 18:
// one HIGH pulse per 9934 us frame, its width taken from the measured duty
// table the firmware serves at /api/device. Nothing here is decorative
// guesswork -- if the trace looks wrong, the table or the speed is wrong.

import { surface } from './charts.js';
import { DIM } from './theme.js';

export interface Waveform {
  /** HIGH fraction of one frame, 0..1. */
  duty: number;
  /** HIGH time in microseconds, for the annotation. */
  highUs: number;
  /** Frame period in microseconds. */
  periodUs: number;
}

/**
 * One square-wave trace, clipped to the canvas and repeated `periods` times.
 * `phase` scrolls it; the scope animates that, the preview leaves it at 0.
 */
function pulse(
  c: CanvasRenderingContext2D,
  W: number,
  H: number,
  duty: number,
  periods: number,
  phase: number,
  colour: string,
  lineWidth: number,
  fill: string | null,
  inset: number,
): void {
  const top = inset;
  const bottom = H - inset;
  const pw = W / periods;
  const start = -phase * pw;

  c.save();
  c.beginPath();
  c.rect(0, 0, W, H);
  c.clip();

  if (fill && duty > 0) {
    c.fillStyle = fill;
    for (let k = -1; k <= periods + 1; k++) {
      c.fillRect(start + k * pw, top, Math.max(duty * pw, 1), bottom - top);
    }
  }

  c.strokeStyle = colour;
  c.lineWidth = lineWidth;
  c.lineJoin = 'miter';
  c.lineCap = 'butt';
  c.beginPath();
  if (duty <= 0) {
    // Speed 0 holds the line low; draw that rather than a degenerate pulse.
    c.moveTo(0, bottom);
    c.lineTo(W, bottom);
  } else {
    c.moveTo(start - pw, top);
    for (let k = -1; k <= periods + 1; k++) {
      const x0 = start + k * pw;
      const x1 = x0 + duty * pw;
      const x2 = x0 + pw;
      c.lineTo(x1, top);
      c.lineTo(x1, bottom);
      c.lineTo(x2, bottom);
      c.lineTo(x2, top);
    }
  }
  c.stroke();
  c.restore();
}

export function drawPreview(canvas: HTMLCanvasElement, wave: Waveform): void {
  const surf = surface(canvas);
  if (!surf) return;
  const { c, W, H } = surf;
  c.fillStyle = '#060a10';
  c.fillRect(0, 0, W, H);
  c.fillStyle = 'rgba(120,160,210,.14)';
  for (let x = 2; x < W; x += 6) {
    for (let j = 0; j <= 2; j++) c.fillRect(x, Math.round(6 + (j * (H - 12)) / 2), 1, 1);
  }
  c.shadowColor = 'rgba(90,165,255,.7)';
  c.shadowBlur = 8;
  pulse(c, W, H, wave.duty, 3, 0, '#3b82f6', 1.8, 'rgba(59,130,246,.12)', 6);
  c.shadowBlur = 4;
  c.shadowColor = 'rgba(200,230,255,.9)';
  pulse(c, W, H, wave.duty, 3, 0, '#d6e8ff', 0.9, null, 6);
  c.shadowBlur = 0;
}

const SCOPE_PERIODS = 4;

export function drawScope(
  canvas: HTMLCanvasElement,
  wave: Waveform,
  phase: number,
  nowMs: number,
): void {
  const surf = surface(canvas);
  if (!surf) return;
  const { c, W, H } = surf;
  const { duty } = wave;
  const pw = W / SCOPE_PERIODS;
  const traceH = H - 34; // the rest is the measurement bracket lane
  const top = 12;
  const bottom = traceH - 12;
  const bracketY = H - 22;

  c.fillStyle = '#060a10';
  c.fillRect(0, 0, W, traceH);

  // 20 x 4 division dot grid, scrolling with the trace.
  const gx = pw / 5;
  const gy = (bottom - top) / 4;
  c.fillStyle = 'rgba(120,165,215,.13)';
  for (let x = -((phase * pw) % gx); x < W; x += gx) {
    for (let j = 0; j <= 4; j++) c.fillRect(Math.round(x), Math.round(top + j * gy), 1, 1);
  }
  c.strokeStyle = 'rgba(120,165,215,.07)';
  c.lineWidth = 1;
  for (let k = 0; k <= SCOPE_PERIODS; k++) {
    const x = Math.round(-phase * pw + k * pw) + 0.5;
    c.beginPath();
    c.moveTo(x, top - 6);
    c.lineTo(x, bottom + 6);
    c.stroke();
  }
  c.setLineDash([2, 5]);
  c.strokeStyle = 'rgba(120,165,215,.16)';
  for (const v of [top, bottom]) {
    c.beginPath();
    c.moveTo(0, v + 0.5);
    c.lineTo(W, v + 0.5);
    c.stroke();
  }
  c.setLineDash([]);

  // Phosphor persistence: fading ghosts of the last few frames.
  for (let k = 4; k >= 1; k--) {
    const alpha = (0.05 + (0.035 * (5 - k)) / 4).toFixed(3);
    pulse(c, W, traceH, duty, SCOPE_PERIODS, phase - k * 0.008,
      `rgba(59,130,246,${alpha})`, 2 + k * 0.9, null, 12);
  }
  c.shadowColor = 'rgba(90,165,255,.85)';
  c.shadowBlur = 15;
  pulse(c, W, traceH, duty, SCOPE_PERIODS, phase, '#3b82f6', 2.4, 'rgba(59,130,246,.10)', 12);
  c.shadowBlur = 7;
  c.shadowColor = 'rgba(200,230,255,.95)';
  pulse(c, W, traceH, duty, SCOPE_PERIODS, phase, '#d6e8ff', 1, null, 12);
  c.shadowBlur = 0;

  if (duty > 0) {
    const start = -phase * pw;
    for (let k = -1; k <= SCOPE_PERIODS + 1; k++) {
      const x = start + k * pw;
      if (x < -3 || x > W + 3) continue;
      const grad = c.createLinearGradient(x, top, x, bottom);
      grad.addColorStop(0, 'rgba(207,228,255,.45)');
      grad.addColorStop(1, 'rgba(207,228,255,0)');
      c.fillStyle = grad;
      c.fillRect(x - 1, top, 2, bottom - top);
      c.fillStyle = '#eaf3ff';
      c.beginPath();
      c.arc(x, top, 2.1, 0, Math.PI * 2);
      c.fill();
    }
  }

  const sweepX = ((nowMs / 2600) % 1) * (W + 140) - 70;
  const sweep = c.createLinearGradient(sweepX - 80, 0, sweepX, 0);
  sweep.addColorStop(0, 'rgba(120,180,255,0)');
  sweep.addColorStop(1, 'rgba(120,180,255,.11)');
  c.fillStyle = sweep;
  c.fillRect(sweepX - 80, 0, 80, traceH);
  c.fillStyle = 'rgba(0,0,0,.10)';
  for (let y = 0; y < traceH; y += 3) c.fillRect(0, y, W, 1);

  c.strokeStyle = 'rgba(120,165,215,.30)';
  c.lineWidth = 1;
  for (const [cx, cy, ux, uy] of [
    [0, 0, 1, 1],
    [W, 0, -1, 1],
    [0, traceH, 1, -1],
    [W, traceH, -1, -1],
  ] as const) {
    c.beginPath();
    c.moveTo(cx + ux * 9.5, cy + uy * 0.5);
    c.lineTo(cx + ux * 0.5, cy + uy * 0.5);
    c.lineTo(cx + ux * 0.5, cy + uy * 9.5);
    c.stroke();
  }

  c.font = '9.5px "JetBrains Mono",monospace';
  c.textAlign = 'left';
  for (const [label, y] of [
    ['5 V', top],
    ['0 V', bottom],
  ] as const) {
    const w = c.measureText(label).width + 9;
    c.fillStyle = 'rgba(6,10,16,.94)';
    c.fillRect(0, y - 6, w, 13);
    c.fillStyle = '#63748c';
    c.fillText(label, 4, y + 3.5);
  }

  // Measurement bracket under one full frame.
  const x0 = -phase * pw + pw;
  const x1 = x0 + duty * pw;
  const x2 = x0 + pw;
  c.strokeStyle = '#39424e';
  c.lineWidth = 1;
  c.beginPath();
  c.moveTo(x0, bracketY);
  c.lineTo(x2, bracketY);
  c.stroke();
  for (const x of [x0, x1, x2]) {
    c.beginPath();
    c.moveTo(x + 0.5, bracketY - 4);
    c.lineTo(x + 0.5, bracketY + 4);
    c.stroke();
  }
  c.textAlign = 'center';
  // Below ~10% the pulse is narrower than its own label.
  if (duty > 0.1) {
    c.fillStyle = '#8fb6e8';
    c.fillText(`${wave.highUs} µs high`, (x0 + x1) / 2, bracketY + 13);
  }
  c.fillStyle = DIM;
  c.fillText(`${wave.periodUs} µs frame`, (x0 + x2) / 2, bracketY - 9);
}

/** Time base shown on the scope: five divisions per frame across four frames. */
export const msPerDivision = (periodUs: number): string => (periodUs / 5000).toFixed(2);
