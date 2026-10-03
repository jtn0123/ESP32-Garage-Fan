// Reading a past moment off the charts: the crosshair, the gestures that drive
// it, and the pin that lets a moment outlive the gesture.
//
// A module of its own for the same reason rail.ts is: this is a CONTROL with a
// gesture, not page wiring, and app.ts is at the 500-line ceiling. It knows
// about the plots, the view-model and the two painters -- nothing about
// fetching, commands or navigation, so nothing here can be reached from a
// network path.

import { $ } from './dom.js';
import { drawAll } from './history_view.js';
import { paint, paintHero, paintStats } from './console.js';
import { pinnedRow, view } from './state.js';
import { PAD_LEFT as L, PAD_RIGHT as R } from './theme.js';


/**
 * Pin the state sentence's box for the duration of a gesture.
 *
 * The plain-English sentence is re-derived on every scrub sample and a scrubbed
 * sentence never wraps to the same number of lines as a live one -- one line
 * SHORTER at 412 (24H), one line LONGER at 320 (7D). Either way #reason changes
 * height, and everything below it moves: the pills, the metric strip, the rail,
 * the sticky chart header and the plot itself, by 17-19 px, while a finger is
 * reading that plot.
 *
 * `height`, not `min-height`: the first attempt reserved a minimum, which stops
 * the box shrinking and does nothing about the case where the new sentence is
 * TALLER -- which is exactly what 320/7D turned out to be. A fixed box with
 * `overflow:hidden` cannot move in either direction, in any font, at any width.
 * The scrub sentence is written to fit inside the shortest live sentence this
 * screen can show (see console.ts::reason), so the clip is a backstop rather
 * than something a reader meets; a truncated tail would still be better than a
 * chart that slides out from under the thumb that touched it.
 *
 * Costs nothing when idle -- the fold budget is why there is no standing
 * min-height on this element.
 */
function freezeReason(hold: boolean): void {
  const reason = $('reason');
  if (!hold) {
    reason.style.height = '';
    reason.style.overflow = '';
    return;
  }
  if (reason.style.height) return;
  reason.style.height = `${Math.ceil(reason.getBoundingClientRect().height)}px`;
  reason.style.overflow = 'hidden';
}

/** The sample nearest a pointer's x, or -1 when it is off either end of the plot. */
function indexAt(clientX: number): number {
  const s = view.series;
  if (!s || s.n < 2) return -1;
  const rect = $<HTMLCanvasElement>('cv_t').getBoundingClientRect();
  const fraction = (clientX - rect.left - L) / (rect.width - L - R);
  // Nearest sample by its time position, not index arithmetic: around a gap
  // the two disagree, and the crosshair must land on a sample that exists.
  let i = 0;
  let best = Infinity;
  for (let k = 0; k < s.n; k++) {
    const d = Math.abs((s.frac[k] ?? 0) - fraction);
    if (d < best) {
      best = d;
      i = k;
    }
  }
  return fraction < -0.02 || fraction > 1.02 ? -1 : i;
}

/** Where the console rests when no pointer is reading the chart: the pin, or now. */
function restIndex(): number {
  return view.series ? pinnedRow(view.series, view.pinTs) : -1;
}

/**
 * Leaving history mode is a full repaint, not a hero repaint: past.ts writes
 * the moment's own values into the rail and the metric strip, and only paint()
 * and paintStats() know how to derive the live ones again.
 */
function repaintLive(): void {
  paint();
  paintStats();
}

/** Move the crosshair and the hero to row `next` (-1 = now), redrawing both. */
function goTo(next: number): void {
  const was = view.scrub;
  freezeReason(next >= 0);
  view.scrub = next;
  drawAll();
  if (next < 0 && was >= 0) repaintLive();
  else paintHero();
}

function scrubAt(clientX: number): void {
  const next = indexAt(clientX);
  // Off the end of the plot is "no reading", which is the resting moment --
  // the pin if there is one -- not necessarily now.
  const target = next < 0 ? restIndex() : next;
  if (target === view.scrub) return;
  goTo(target);
}

/** The gesture is over: back to the pinned moment if there is one, else to now. */
export function endScrub(): void {
  const rest = restIndex();
  if (view.scrub === rest) {
    freezeReason(rest >= 0);
    return;
  }
  goTo(rest);
}

/**
 * Hold the moment under a click or a tap until something releases it.
 *
 * Hover and drag only PREVIEW: the moment you are reading vanishes the instant
 * the pointer leaves the plot or the finger lifts, so the hero's history mode
 * could never be looked at -- on a desk the mouse has to cross the hero's own
 * controls to get there, on a phone the hero is above the thumb. A click is
 * the gesture that already means "this one". No clock, no pin: a moment is
 * pinned by its timestamp, and before SNTP there is none to hold it by.
 */
function pinAt(clientX: number): void {
  const s = view.series;
  const i = indexAt(clientX);
  const t = s && i >= 0 ? s.ts(i) : null;
  if (t === null) return;
  view.pinTs = t;
  // Through goTo even on the row the hover already shows: holding changes the
  // crosshair's ink as well as the hero, and only drawAll repaints that.
  goTo(i);
}

/** "Back to now": drop the pin and the preview together. */
export function backToNow(): void {
  view.pinTs = null;
  endScrub();
}

/**
 * Re-find the pinned moment after the series has been rebuilt.
 *
 * Called by app.ts::loadHistory before it redraws. A pin that has scrolled out
 * of the window (the 24 h ring moved on, or a shorter range was chosen) is let
 * go, and the console goes back to now with it.
 */
export function settleScrub(): void {
  if (view.pinTs === null) return;
  const k = restIndex();
  if (k < 0) {
    view.pinTs = null;
    if (view.scrub >= 0) {
      freezeReason(false);
      view.scrub = -1;
      repaintLive();
    }
    return;
  }
  view.scrub = k;
}

/**
 * Which way a touch gesture over the plots is going, decided once.
 *
 * The touchmove handler used to call preventDefault() unconditionally, so a
 * vertical swipe that started anywhere over the charts could not scroll the
 * page: on a phone the plot stack is ~400 px of screen, which made it a dead
 * zone you had to reach around. A swipe is either a scrub (horizontal -- that
 * IS the gesture, "drag across to read any moment") or a scroll (vertical),
 * and the two cannot both win.
 *
 * The axis is locked on the first movement past the slop radius and never
 * revisited for that gesture: a scrub that drifts up must not suddenly hand
 * the page a scroll under the finger, and a scroll that drifts sideways must
 * not start rewriting the hero while it flies past.
 */
const AXIS_SLOP_PX = 10; // ~1.5 mm; below this a touch has no direction yet
let touchOrigin: { x: number; y: number } | null = null;
let touchAxis: 'undecided' | 'scrub' | 'scroll' = 'undecided';

function onTouchStart(e: TouchEvent): void {
  const t = e.touches[0];
  touchOrigin = t ? { x: t.clientX, y: t.clientY } : null;
  touchAxis = 'undecided';
}

function onTouchMove(e: TouchEvent): void {
  const t = e.touches[0];
  if (!t || !touchOrigin) return;
  if (touchAxis === 'undecided') {
    const dx = Math.abs(t.clientX - touchOrigin.x);
    const dy = Math.abs(t.clientY - touchOrigin.y);
    if (Math.max(dx, dy) < AXIS_SLOP_PX) return;
    touchAxis = dx > dy ? 'scrub' : 'scroll';
  }
  if (touchAxis !== 'scrub') return; // the browser owns this one: let it scroll
  scrubAt(t.clientX);
  // Only now, and only for a gesture already committed to scrubbing: this is
  // what keeps the page still while a finger reads along the chart.
  e.preventDefault();
}

/**
 * End of a touch gesture -- including touchcancel, which is not a rare case.
 *
 * The browser fires it whenever it takes the gesture over for scrolling, and
 * only touchend was handled: a swipe that began as a scrub and turned into a
 * scroll left the crosshair frozen on the chart and the hero showing a past
 * temperature under a "NOW"-less stamp, with no touch left to clear it.
 */
function onTouchRelease(): void {
  touchOrigin = null;
  touchAxis = 'undecided';
  endScrub();
}

/**
 * Wire every way of reading the chart: pointer for a mouse or a stylus, touch
 * with the axis lock above.
 *
 * Pointer events, not mouse events: a phone browser reports a real mouse or a
 * stylus only through these, and `mousemove` on a touch device is a
 * compatibility event that arrives after the fact, at the tap position. The
 * touch pointers are skipped because the touch handlers own them -- scrubbing
 * from a raw pointermove would bypass the axis lock and bring the
 * trapped-page bug straight back.
 */
export function attachScrub(plots: HTMLElement): void {
  plots.addEventListener('pointermove', (e) => {
    if ((e as PointerEvent).pointerType !== 'touch') scrubAt((e as PointerEvent).clientX);
  });
  plots.addEventListener('pointerleave', (e) => {
    if ((e as PointerEvent).pointerType !== 'touch') endScrub();
  });
  plots.addEventListener('touchstart', (e) => onTouchStart(e as TouchEvent), { passive: true });
  plots.addEventListener('touchmove', (e) => onTouchMove(e as TouchEvent), { passive: false });
  plots.addEventListener('touchend', onTouchRelease);
  plots.addEventListener('touchcancel', onTouchRelease);
  // A click is a mouse click AND a touch tap: a tap that never moved produces
  // one, at the tap position, after touchend has already ended the preview. A
  // drag does not -- so dragging still previews and lifting still returns.
  plots.addEventListener('click', (e) => pinAt((e as MouseEvent).clientX));

  // Every way back. The hero's button, a compact one in the chart header for a
  // phone scrolled down to the plots, and Escape from anywhere on the console.
  $('bnow').addEventListener('click', backToNow);
  $('chnow').addEventListener('click', backToNow);
  document.addEventListener('keydown', (e) => {
    if (e.key !== 'Escape' || view.screen !== 'console') return;
    if (view.pinTs === null && view.scrub < 0) return;
    backToNow();
  });
}
