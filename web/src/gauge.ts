// The differential gauge under the hero: a track from "garage much cooler than
// the yard" to "much hotter", the release/engage band, the needle, a numbered
// scale, and the state where a winter limit -- not the differential -- is in
// charge.
//
// Split out of console.ts when the scale and the held state arrived. The maths
// is pure and exported for tests/gauge.test.ts; paintGauge is the only part
// that touches the DOM.

import { $, el, show } from './dom.js';
import { deg } from './reason.js';
import type { AutoLimit, DeviceState } from './types.js';

/* ------------------------------------------------------------------ geometry */

/** Track positions, in percent of the gauge width. */
const ZERO_PCT = 46; // a 0 °F differential
const MIN_PCT = 2;
const MAX_PCT = 97;
/**
 * The long-standing scale: -6.9 to +8.0 °F across the track, which holds the
 * default +1.5/+2.5 band with room either side.
 */
const PCT_PER_F = 6.4;
/** Room kept past the engage point, °F, so it never sits on the end cap. */
const ENGAGE_ROOM_F = 2;

export interface GaugeGeometry {
  /** Percent of the track per °F. */
  perF: number;
  /** The differentials at the two ends of the track, °F. */
  lo: number;
  hi: number;
}

/**
 * The track's range, from the one setting that can outgrow it.
 *
 * Engage goes up to +20 °F in Settings, and the old fixed 6.4 %/°F pinned
 * anything past +8 to the end cap -- harmless with no numbers on the track,
 * a lie with them. Past that point the whole scale compresses to fit; below
 * it nothing moves, so every ordinary install draws exactly as before.
 */
export function gaugeGeometry(onF: number): GaugeGeometry {
  const fit = (MAX_PCT - ZERO_PCT) / (Math.max(onF, 0) + ENGAGE_ROOM_F);
  const perF = Math.min(PCT_PER_F, fit);
  return { perF, lo: (MIN_PCT - ZERO_PCT) / perF, hi: (MAX_PCT - ZERO_PCT) / perF };
}

/** A differential's place on the track, clamped to the end caps. */
export function gaugePos(v: number, g: GaugeGeometry): number {
  return Math.max(MIN_PCT, Math.min(MAX_PCT, ZERO_PCT + v * g.perF));
}

/** Which end cap, if any, a differential is pinned against. */
export function pinned(v: number, g: GaugeGeometry): 'lo' | 'hi' | null {
  if (v < g.lo) return 'lo';
  if (v > g.hi) return 'hi';
  return null;
}

/* --------------------------------------------------------------------- scale */

/** Candidate label steps, °F; the last is the fallback for a sliver of a gauge. */
const STEPS = [1, 2, 5] as const;
const COARSEST_STEP = 10;
/** More numbers than this along one bar reads as a ruler, not a gauge. */
const MAX_LABELS = 8;
/** Centre-to-centre room a "+10" needs at 9 px mono, with air either side. */
const MIN_LABEL_GAP_PX = 34;
/**
 * How close, in px, a number's centre may come to a release/engage tick.
 * Those ticks cross the number row on their way to their own labels, and a
 * threshold on an even degree (engage +2.0) would otherwise strike its
 * number through. Half a two-glyph label plus breathing room -- just enough
 * that "+2" still fits between the default +1.5/+2.5 ticks on a 320 px phone.
 */
const LABEL_CLEAR_PX = 8;
/** Used before layout (jsdom, a hidden screen): a mid-sized phone. */
const FALLBACK_WIDTH_PX = 380;

export interface GaugeTick {
  v: number;
  pct: number;
  major: boolean;
  /** null on minor ticks, and on majors that would collide with a threshold. */
  label: string | null;
}

/**
 * The labelled step: the finest that keeps at most eight numbers on the track
 * and ~34 px between them. 2 °F on every width at the default range -- a 320 px
 * phone still gets 37 px per step.
 */
export function gaugeStep(g: GaugeGeometry, widthPx: number): number {
  const pxPerF = ((widthPx || FALLBACK_WIDTH_PX) * g.perF) / 100;
  for (const step of STEPS) {
    const count = Math.floor(g.hi / step) - Math.ceil(g.lo / step) + 1;
    if (count <= MAX_LABELS && step * pxPerF >= MIN_LABEL_GAP_PX) return step;
  }
  return COARSEST_STEP;
}

/** "+4" / "0" / "−2", with a real minus sign: a hyphen is half as wide as a plus. */
export function scaleLabel(v: number): string {
  if (v > 0) return `+${v}`;
  return v < 0 ? `−${-v}` : '0';
}

/**
 * Every tick on the track: majors at the labelled step, minors halfway (or at
 * each degree on a 5 °F step). `avoid` holds the release/engage differentials;
 * a major too close to one keeps its tick and loses its number -- the
 * threshold's own label states its value, so nothing is lost.
 */
export function gaugeTicks(g: GaugeGeometry, widthPx: number, avoid: readonly number[]): GaugeTick[] {
  const step = gaugeStep(g, widthPx);
  const minor = step === 5 ? 1 : step / 2;
  const pxPerF = ((widthPx || FALLBACK_WIDTH_PX) * g.perF) / 100;
  const clearF = LABEL_CLEAR_PX / pxPerF;
  const ticks: GaugeTick[] = [];
  // Integer multiples of the minor step, so float drift cannot add or drop one.
  for (let k = Math.ceil(g.lo / minor); k * minor <= g.hi; k++) {
    const v = k * minor;
    const major = Number.isInteger(v / step);
    const clash = avoid.some((a) => Math.abs(a - v) < clearF);
    ticks.push({ v, pct: gaugePos(v, g), major, label: major && !clash ? scaleLabel(v) : null });
  }
  return ticks;
}

/* ---------------------------------------------------------------- held state */

/**
 * The words on the gauge while a limit is in charge, or null when the
 * differential is. Only in auto -- the firmware reports no limit in manual,
 * but the gauge should not assert one even if a stale frame says otherwise.
 */
export function heldTag(s: DeviceState): { kind: AutoLimit; text: string } | null {
  if (!s.auto || s.limit === null) return null;
  return s.limit === 'floor'
    ? { kind: 'floor', text: `HELD BY LOW LIMIT ${deg(s.floor_f)}` }
    : { kind: 'start', text: `HELD BY START POINT ${deg(s.start_f)}` };
}

/* ------------------------------------------------------------------ painting */

/** The held tag's ▴ centre, px in from the edge it opens on (padding + half a glyph). */
const TAG_POINTER_PX = 10;

/** The scale is rebuilt only when something it depends on changes. */
let scaleKey = '';

function paintScale(ticks: readonly GaugeTick[]): void {
  const key = ticks.map((t) => `${t.v}:${t.label ?? ''}`).join(',');
  if (key === scaleKey) return;
  scaleKey = key;
  $('gscale').replaceChildren(
    ...ticks.flatMap((t) => {
      const tick = el('i', { className: t.major ? 'maj' : '' });
      tick.style.left = `${t.pct}%`;
      if (t.label === null) return [tick];
      const num = el('b', { textContent: t.label });
      num.style.left = `${t.pct}%`;
      return [tick, num];
    }),
  );
}

/**
 * `delta` is the differential the hero shows, °F, or null when there is none;
 * `live` is false while a scrub shows a past one.
 */
export function paintGauge(s: DeviceState, delta: number | null, live: boolean): void {
  const g = gaugeGeometry(s.on_f);
  const pos = (v: number): number => gaugePos(v, g);
  const gauge = $('gauge');
  paintScale(gaugeTicks(g, gauge.offsetWidth, [s.off_f, s.on_f]));

  const band = $('gband');
  band.style.left = `${pos(s.off_f)}%`;
  band.style.width = `${Math.max(0, pos(s.on_f) - pos(s.off_f))}%`;
  $('gtr').style.left = `${pos(s.off_f)}%`;
  $('gte').style.left = `${pos(s.on_f)}%`;
  const relLabel = $('glr');
  relLabel.style.left = `${pos(s.off_f)}%`;
  relLabel.textContent = `RELEASE +${s.off_f} ◂`;
  const engLabel = $('gle');
  engLabel.style.left = `${pos(s.on_f)}%`;
  engLabel.textContent = `▸ ENGAGE +${s.on_f}`;

  const marker = $('gmark');
  marker.style.left = `${delta === null ? ZERO_PCT : pos(delta)}%`;
  marker.style.opacity = delta === null ? '0.25' : '1';
  // Off the end of a numbered scale the needle would read as the last number;
  // a chevron says "further than this" (the hero carries the real value).
  const pin = delta === null ? null : pinned(delta, g);
  marker.className = pin === null ? '' : `pin${pin}`;

  const held = live ? heldTag(s) : null;
  gauge.className = held ? `held ${held.kind}` : '';
  const tag = $('gheld');
  show(tag, held !== null);
  if (held === null) return;
  // A callout from the needle: the reading is real, it is just not in charge.
  // The pointer sits under the needle; past the point where the tag would run
  // off the right end it flips to the tag's far end.
  const width = gauge.offsetWidth;
  const x = (width * (delta === null ? ZERO_PCT : pos(delta))) / 100;
  tag.textContent = `▴ ${held.text}`;
  let left = x - TAG_POINTER_PX;
  if (left + tag.offsetWidth > width) {
    tag.textContent = `${held.text} ▴`;
    left = x + TAG_POINTER_PX - tag.offsetWidth;
  }
  // Inside the gauge either way: a needle pinned to an end cap would push the
  // tag a few px past it, and on a 320 px phone there is nothing past it.
  tag.style.left = `${Math.max(0, Math.min(left, width - tag.offsetWidth))}px`;
}
