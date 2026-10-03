// The history area: the caption, the per-row readouts, and the draw pass that
// turns view.series into the stack of plots.
//
// Split out of console.ts when the scrub readout arrived and pushed that file
// past the 500-line ceiling. The boundary is the one the screen already has:
// console.ts paints the live device (hero, gauge, PWM, status bits), this
// paints the recorded past. Both read the shared view-model in state.ts and
// write only to the DOM.

import { drawBattery, drawFanSpeed, drawPower, drawSimple } from './chart_rows.js';
import { drawTemperature, holdCrosshair, shadesNights } from './charts.js';
import { $, at, el } from './dom.js';
import { ago, hoursMinutes, moment, rangeLabel, rangeNoun } from './format.js';
import { chartLimits, limitOverlay } from './limit_lines.js';
import type { Series } from './series.js';
import { pinnedRow, sampleIndex, view } from './state.js';
import { drawAxis } from './time_axis.js';
import { OR, OUT, SERIES_COLOURS } from './theme.js';

/** The line sample each legend entry opens with, matching the stroke it names. */
const STROKE = { solid: '——', dashed: '╌╌', dotted: '┄┄' } as const;

/** One entry of a series legend: a coloured line sample and the sensor name. */
function legendEntry(name: string, colour: string, stroke: keyof typeof STROKE): HTMLElement {
  const b = el('b', { textContent: `${STROKE[stroke]} ${name}` });
  b.style.color = colour;
  return b;
}

/** The overnight stripes' key: a swatch of the tint, since they are not a line. */
function nightEntry(): HTMLElement {
  const b = el('b', { className: 'night' });
  b.append(el('i'), 'NIGHT');
  return b;
}

/** Which line is which sensor, on the rows that carry more than one. */
function paintLegends(s: Series): void {
  $('tleg').replaceChildren(
    legendEntry('GARAGE', OR, 'solid'),
    legendEntry('OUTSIDE', OUT, 'dashed'),
    // Only while the stripes are drawn: past an hour per row they are not.
    ...(shadesNights(s) ? [nightEntry()] : []),
    // Only the limits switched on. The value rides on the chart's own tag.
    ...chartLimits(view.state).map((l) => legendEntry(l.name, l.colour, 'dotted')),
  );
  $('hleg').replaceChildren(legendEntry('GARAGE', SERIES_COLOURS.humidity, 'solid'));
}

/**
 * How far back sample i is, in minutes.
 *
 * Real timestamp difference, not index * interval: a series with holes in it
 * (reboots, outages) has no constant step, and neither does a decimated long
 * range -- 60 days arrive ~5 hours apart. Index arithmetic understates the age
 * across every one of those.
 */
export function minutesBack(s: Series, i: number): number {
  const t = s.ts(i);
  const tNow = s.ts(s.n - 1);
  if (t !== null && tNow !== null) return (tNow - t) / 60;
  return ((s.n - 1 - i) * (view.history?.interval_s ?? 300)) / 60;
}

/**
 * The chart header doubles as the scrub readout.
 *
 * A crosshair with no legible time answers "which moment is this?" nowhere on
 * a phone: the hero stamp has usually scrolled off the top by the time a thumb
 * reaches the plots, and the finger covers whatever sits under it. The title
 * slot sits directly above the plots, sticks to the top of the screen at phone
 * widths, and is never under the hand -- so it carries the moment while
 * scrubbing and goes back to naming the range when the gesture ends.
 */
export function paintChartTitle(): void {
  const title = $('chtitle');
  const s = view.series;
  const i = sampleIndex();
  const t = view.scrub >= 0 && s && i >= 0 ? s.ts(i) : null;
  if (t === null || !s) {
    title.textContent = rangeLabel(view.days);
    title.className = 'ct';
    return;
  }
  // No arrows around it. `◂ 20:09 ▸` drew a 7 px glyph pair that read as a
  // prev/next stepper for the sample, 13 px from a real control, and stepped
  // nothing -- the drag already does that job. Bracketing a RANGE (the deadband
  // labels) is what that glyph is for; bracketing a value invents a button.
  title.textContent = `${moment(t, view.days)} · ${ago(minutesBack(s, i))}`;
  title.className = 'ct scrub';
}

const CHART_HINT =
  'band = how much hotter the garage is than the yard · drag across to read any moment';

/**
 * What the temperature row's caption says, which is not always the hint.
 *
 * A range with nothing in it, or one the card can only half fill, rendered as
 * a blank -- or as a chart squeezed into its right-hand quarter -- with no
 * explanation anywhere. On a phone there was nowhere for an explanation to
 * land at all, because the caption is hidden at that width. Anything that
 * EXPLAINS the picture is marked `note`, which is the class that keeps it
 * visible on a phone; the standing hint stays desktop-only.
 */
export function paintCaption(): void {
  const cap = $('tcap');
  const s = view.series;
  const note = (text: string): void => {
    cap.textContent = text;
    cap.className = 'cd note';
  };
  if (view.historyError) return note(view.historyError);
  if (!s || s.n === 0) {
    return note('no samples in this range yet — the controller logs one every 5 minutes');
  }
  const first = s.ts(0);
  const last = s.ts(s.n - 1);
  const coveredH = first !== null && last !== null ? (last - first) / 3600 : null;
  // Two thirds: a card that has been logging for most of the window does not
  // need a caveat, and the newest sample always sits a step short of the edge.
  if (coveredH !== null && coveredH < view.days * 24 * 0.67) {
    return note(
      `the card only goes back ${hoursMinutes(coveredH * 3600)} — the rest of this ` +
        `${rangeNoun(view.days)} window is older than anything it holds`,
    );
  }
  cap.textContent = CHART_HINT;
  cap.className = 'cd';
}

/** "index 93 · raw 30125" / "warming up · raw 29850" / '' -- one gas row. */
function gasReadout(idx: number | null, raw: number | null): string {
  if (idx === null && raw === null) return '';
  const rawPart = raw === null ? '' : ` · raw ${raw.toFixed(0)}`;
  return idx !== null && idx > 0 ? `index ${idx.toFixed(0)}${rawPart}` : `warming up${rawPart}`;
}

/**
 * "speed 7" / "off" / '' -- the fan row.
 *
 * Three outcomes, and the empty one is not the same as "off": a range whose
 * rows carry no speed column at all (the pre-1.14.23 CSV width) must say
 * nothing rather than claim the fan was stopped.
 */
export function speedReadout(spd: number | undefined): string {
  if (spd === undefined) return '';
  return spd > 0 ? `speed ${spd}` : 'off';
}

/** "4.19 V" / "4.19 V ⚡ charging" / '' -- the battery row. */
/**
 * What the meter saw in one bucket: "45.1 W", "4.3–45.6 W" when the draw
 * moved inside those five minutes, and "· cycling ×3" when those swings were
 * confirmed run/stop flips. The snapshot alone is what let a fan cycling
 * every minute read as an ordinary reading at whatever instant it was taken.
 */
export function powerReadout(
  w: number | null,
  flips: number | null,
  lo: number | null = null,
  hi: number | null = null,
): string {
  if (w === null) return '';
  // A range worth printing: more than a watt of spread inside one bucket.
  const spread = lo !== null && hi !== null && hi - lo > 1;
  const base = spread ? `${lo!.toFixed(1)}–${hi!.toFixed(1)} W` : `${w.toFixed(1)} W`;
  return flips !== null && flips > 0 ? `${base} · cycling ×${flips}` : base;
}

export function battReadout(volts: number | null, charging: boolean): string {
  if (volts === null) return '';
  return `${volts.toFixed(2)} V${charging ? ' ⚡ charging' : ''}`;
}

function paintReadouts(): void {
  const s = view.series;
  const i = sampleIndex();
  if (!s || i < 0) return;
  const tf = at(s.tf, i);
  const of = at(s.of, i);
  $('roT').textContent =
    (tf === null ? '–' : `${tf.toFixed(1)}°`) + (of === null ? '' : ` · out ${of.toFixed(1)}°`);
  $('roS').textContent = speedReadout(s.spd.length ? s.spd[i] : undefined);
  const rhNow = at(s.rh, i);
  $('roH').textContent = rhNow === null ? '' : `${rhNow.toFixed(0)}%`;
  const hpa = at(s.hpa, i);
  $('roP').textContent = hpa === null ? '' : `${hpa.toFixed(1)} mb`;
  $('roB').textContent = battReadout(at(s.bv, i), s.chg[i] === 1);
  const w = at(s.w, i);
  $('roW').textContent = powerReadout(w, at(s.flips, i), at(s.wmin, i), at(s.wmax, i));
  $('roV').textContent = gasReadout(at(s.voc, i), at(s.vocr, i));
  $('roN').textContent = gasReadout(at(s.nox, i), at(s.noxr, i));
}

/**
 * Blank every plot. The caption says why (paintCaption).
 *
 * Used when a range cannot be served: leaving the previous range's picture
 * under the new range's title is the failure the firmware's 503 exists to
 * prevent, and silently keeping it undid that at the last step.
 */
function clearPlots(): void {
  // Every plot, the power and gas rows included: missing from this list, they
  // kept the previous range's picture under the new range's title.
  for (const id of ['cv_t', 'cv_s', 'cv_h', 'cv_p', 'cv_b', 'cv_w', 'cv_v', 'cv_n', 'cv_ax']) {
    const c = document.getElementById(id) as HTMLCanvasElement | null;
    const ctx = c?.getContext('2d');
    if (c && ctx) ctx.clearRect(0, 0, c.width, c.height);
  }
}

export function drawAll(): void {
  if (view.screen !== 'console') return;
  paintCaption();
  paintChartTitle();
  const s = view.series;
  if (!s) {
    if (view.historyError) clearPlots();
    return;
  }
  paintLegends(s);
  holdCrosshair(view.scrub >= 0 && view.scrub === pinnedRow(s, view.pinTs));
  drawTemperature($<HTMLCanvasElement>('cv_t'), s, view.scrub, view.boots,
    limitOverlay(s, chartLimits(view.state)));
  if (view.rows.fan) drawFanSpeed($<HTMLCanvasElement>('cv_s'), s, view.scrub);
  if (view.rows.humidity) {
    drawSimple($<HTMLCanvasElement>('cv_h'), s, s.rh, SERIES_COLOURS.humidity,
      'no humidity data', view.scrub, { lowest: 0 });
  }
  if (view.rows.pressure) {
    // 0.5 hPa floor: the ticks carry one decimal, so a smaller range is still
    // legible in the labels and does not need flattening.
    drawSimple($<HTMLCanvasElement>('cv_p'), s, s.hpa, SERIES_COLOURS.pressure,
      'no pressure data', view.scrub, { minSpan: 0.5 });
  }
  if (view.rows.battery) drawBattery($<HTMLCanvasElement>('cv_b'), s, view.scrub);
  if (view.rows.power) drawPower($<HTMLCanvasElement>('cv_w'), s, view.scrub);
  // Gas rows plot the INDEX once the algorithm produces one, and the raw
  // ticks before that -- the user asked to watch the warm-up, not to stare at
  // a flat zero for the hours Sensirion's blackout lasts.
  if (view.rows.voc) {
    const warming = !s.voc.some((v) => v !== null && v > 0);
    drawSimple($<HTMLCanvasElement>('cv_v'), s, warming ? s.vocr : s.voc, SERIES_COLOURS.voc,
      'no VOC sensor data', view.scrub, { lowest: 0 });
  }
  if (view.rows.nox) {
    const warming = !s.nox.some((v) => v !== null && v > 0);
    drawSimple($<HTMLCanvasElement>('cv_n'), s, warming ? s.noxr : s.nox, SERIES_COLOURS.nox,
      'no NOx sensor data', view.scrub, { lowest: 0 });
  }
  drawAxis($<HTMLCanvasElement>('cv_ax'), s);
  paintReadouts();
}
