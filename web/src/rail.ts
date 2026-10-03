// The speed rail: twelve blocks that are one control, not twelve.
//
// Split from app.ts/console.ts because the rail stopped being a row of buttons
// the moment it became draggable, and because app.ts was at the 500-line
// ceiling. It owns three things: the blocks, the gesture, and painting them
// (the paint has to read a PENDING selection that no device frame knows about).
//
// Why a drag at all: at 320 px the twelve blocks measure 20x34, which is under
// half a fingertip on the one control you reach for while standing in a garage
// -- and a mis-tap here changes the fan speed. Grown to 44 px tall they are
// still only 24 px wide, and twelve 44 px-wide targets cannot fit in 288 px
// without overlapping, so target size alone cannot solve it. A drag can: the
// thumb sweeps, the number follows it live, and you correct before letting go.
//
// The command is sent ONCE, on release. Sending per step would have pushed nine
// speed changes at the fan during one sweep -- the fan's own controller ramps,
// so that is both noise on the wire and audible chatter in the garage.

import { $, el } from './dom.js';
import { view } from './state.js';
import { AC, PAST } from './theme.js';

/** Movement that turns a press into a sweep. Below it, the tap wins. */
const DRAG_SLOP_PX = 8;

let onPick: ((speed: number) => void) | null = null;
/** Set while a drag is committing, so the click it also produces is ignored. */
let swallowClick = false;

/**
 * Which block the pointer is over, 1..12, or null when it is off the rail.
 *
 * Orientation-aware because the rail has two of them: a row on a phone, and a
 * bottom-up column on a desk (`flex-direction:column-reverse`, speed 1 at the
 * bottom). Reading x in both would have made every desktop sweep return the
 * step under the wrong axis.
 */
function stepAt(clientX: number, clientY: number): number | null {
  const stack = $('stack');
  const rect = stack.getBoundingClientRect();
  const n = stack.children.length;
  if (rect.width <= 0 || rect.height <= 0 || n === 0) return null;
  const vertical = rect.height > rect.width;
  const along = vertical ? (rect.bottom - clientY) / rect.height : (clientX - rect.left) / rect.width;
  const k = Math.floor(along * n);
  if (k < 0 || k >= n) return null;
  return k + 1;
}

/**
 * Paint the blocks and the big number.
 *
 * `view.railPick` is the speed under a live finger: it outranks the device's
 * own speed for as long as the gesture lasts, otherwise a poll landing
 * mid-sweep would snap the rail back to what the fan is still doing.
 */
/** "off" / "raw" / "7" -- the big number over the rail; '–' for a moment with no speed logged. */
function railLabel(speed: number | null): string {
  if (speed === null) return '–';
  if (speed === 0) return 'off';
  // Negative means the fan is on a raw duty this table cannot name: the
  // controller accepts one over MQTT, and printing a step number for it would
  // be a guess.
  if (speed < 0) return 'raw';
  return String(speed);
}

/** Lit blocks in the live blue, or in the history tone for a logged moment. */
// The sand is brighter than the blue at the same alpha, so its ramp starts and
// climbs lower: a logged speed should not out-shout the live one it stands in for.
const TONE = {
  live: { rgb: '59,130,246', edge: AC, base: 0.2, step: 0.055 },
  past: { rgb: '217,179,108', edge: PAST, base: 0.1, step: 0.04 },
};

/** Border of block `n`: the selected step, a lit one below it, or unlit. */
function blockBorder(n: number, shown: number, tone: (typeof TONE)['live']): string {
  if (n === shown) return tone.edge;
  return n <= shown && shown > 0 ? `rgba(${tone.rgb},.4)` : '#1a2029';
}

/**
 * `past` paints a LOGGED speed (past.ts, history mode): the blocks
 * take the history tone and the label says THEN, so a rail showing what the fan
 * did at 03:00 cannot be read as what it is doing now. A live finger still
 * outranks it -- touching the rail is a command, and commands are about now.
 */
export function paintRail(speed: number | null, past = false): void {
  const picking = view.railPick !== null;
  const history = past && !picking;
  const shown = view.railPick ?? speed;
  const tone = history ? TONE.past : TONE.live;
  const num = $('railnum');
  num.textContent = railLabel(shown);
  num.classList.toggle('pick', picking);
  num.classList.toggle('past', history);
  $('raillab').textContent = history ? 'SPEED THEN' : 'SPEED / 12';
  const level = shown ?? 0;
  Array.from($('stack').children).forEach((child, k) => {
    const b = child as HTMLElement;
    const n = k + 1;
    const lit = n <= level && level > 0;
    b.style.background = lit ? `rgba(${tone.rgb},${tone.base + tone.step * n})` : '#12161d';
    b.style.borderColor = blockBorder(n, level, tone);
  });
}

function preview(speed: number | null): void {
  if (view.railPick === speed) return;
  view.railPick = speed;
  paintRail(view.state?.speed ?? 0);
}

function onPointerDown(e: PointerEvent): void {
  dragFrom = { x: e.clientX, y: e.clientY };
  sweeping = false;
  // Preview immediately, before any movement: a press that has not been
  // released yet is a question ("this one?"), and answering it on screen is
  // what lets a thumb that landed one block off slide over and fix it instead
  // of changing the fan speed and then changing it back.
  preview(stepAt(e.clientX, e.clientY));
}

function onPointerMove(e: PointerEvent): void {
  if (!dragFrom) return;
  if (!sweeping) {
    const dx = Math.abs(e.clientX - dragFrom.x);
    const dy = Math.abs(e.clientY - dragFrom.y);
    if (Math.max(dx, dy) < DRAG_SLOP_PX) return;
    // Same axis rule as the charts, along the rail's OWN axis: a swipe across
    // the phone's horizontal rail is someone scrolling the page, not someone
    // setting a speed. On the desk column it is the other way round.
    const rect = $('stack').getBoundingClientRect();
    const across = rect.height > rect.width ? dx > dy : dy > dx;
    if (across) {
      dragFrom = null;
      return;
    }
    sweeping = true;
  }
  const step = stepAt(e.clientX, e.clientY);
  if (step !== null) preview(step);
  e.preventDefault();
}

function onPointerUp(e: PointerEvent): void {
  const picked = sweeping ? view.railPick : null;
  dragFrom = null;
  sweeping = false;
  preview(null);
  if (picked === null) return; // a tap: the button's own click handler owns it
  swallowClick = true;
  onPick?.(picked);
  e.preventDefault();
}

function onPointerCancel(): void {
  // The browser took the gesture (a scroll). Nothing was chosen, so nothing is
  // sent -- and the rail goes back to showing what the fan is actually doing.
  dragFrom = null;
  sweeping = false;
  preview(null);
}

let dragFrom: { x: number; y: number } | null = null;
let sweeping = false;

/** Build the twelve blocks and wire both ways of choosing a speed. */
export function buildRail(pick: (speed: number) => void): void {
  onPick = pick;
  const stack = $('stack');
  stack.replaceChildren(
    ...Array.from({ length: 12 }, (_, k) => {
      const n = k + 1;
      return el('button', {
        title: `set speed ${n}`,
        onclick: () => {
          if (swallowClick) {
            swallowClick = false;
            return;
          }
          pick(n);
        },
      });
    }),
  );
  stack.addEventListener('pointerdown', (e) => onPointerDown(e as PointerEvent));
  stack.addEventListener('pointermove', (e) => onPointerMove(e as PointerEvent));
  stack.addEventListener('pointerup', (e) => onPointerUp(e as PointerEvent));
  stack.addEventListener('pointercancel', onPointerCancel);
  stack.addEventListener('pointerleave', onPointerCancel);
}
