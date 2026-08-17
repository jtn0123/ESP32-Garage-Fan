// Reading a past moment off the charts: the crosshair, and the gestures that
// drive it.
//
// A module of its own for the same reason rail.ts is: this is a CONTROL with a
// gesture, not page wiring, and app.ts is at the 500-line ceiling. It knows
// about the plots, the view-model and the two painters -- nothing about
// fetching, commands or navigation, so nothing here can be reached from a
// network path.

import { $ } from './dom.js';
import { drawAll } from './history_view.js';
import { paintHero } from './console.js';
import { view } from './state.js';
import { PAD_LEFT as L, PAD_RIGHT as R } from './theme.js';


/**
 * Hold the layout above the charts still for the duration of a gesture.
 *
 * The plain-English state sentence is re-derived on every scrub sample, and a
 * scrubbed sentence is not the same length as a live one: at 24H it came out
 * one line shorter, which moved #reason, which moved everything below it --
 * including the sticky chart header and the plot itself, 17 px up, WHILE a
 * finger was reading it. Reserving the height it already has costs nothing
 * when idle (the fold budget is why it has no standing min-height) and makes
 * the reflow unobservable during the one moment it would be felt.
 */
function freezeReason(hold: boolean): void {
  const reason = $('reason');
  if (!hold) {
    reason.style.minHeight = '';
    return;
  }
  if (reason.style.minHeight) return;
  reason.style.minHeight = `${Math.ceil(reason.getBoundingClientRect().height)}px`;
}

function scrubAt(clientX: number): void {
  const s = view.series;
  if (!s || s.n < 2) return;
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
  const next = fraction < -0.02 || fraction > 1.02 ? -1 : i;
  if (next === view.scrub) return;
  freezeReason(next >= 0);
  view.scrub = next;
  drawAll();
  paintHero();
}

export function endScrub(): void {
  freezeReason(false);
  if (view.scrub === -1) return;
  view.scrub = -1;
  drawAll();
  paintHero();
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
}
