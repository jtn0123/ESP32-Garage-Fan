// The settings jump bar: one tab per settings group, pinned to the top of the
// screen while the groups scroll under it, marking the group in view and
// scrolling to a group on tap.
//
// Settings is ~6,000 px of scrolling on a phone, and before this the only way
// to reach UPDATE was to flick past everything above it. The tabs are built
// from the same Group list the renderer draws, so a group added to the spec
// gains its tab without a second list to keep in step -- and sectionId() is
// the one place both ends agree on what a group's element is called.

import { $, el, markOverflow } from './dom.js';
import type { Group } from './settings.js';

/** DOM id for a group's section, derived from its title. */
export function sectionId(title: string): string {
  // The first pass leaves at most one '-' at each end, so the trim needs no
  // quantifier -- and so no backtracking on a long run of separators.
  const slug = title.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
  return `set-${slug}`;
}

/**
 * Which section is "in view": the last one whose top has reached `line` (the
 * bar's bottom edge plus a little lead), or the first while none has.
 *
 * At the very end of the page the last section wins even though its top never
 * climbs that far. UPDATE is shorter than a screen, so on scroll position
 * alone its tab could never light -- a bar that cannot point at the section
 * you are reading is a bar that is wrong exactly where you check it.
 */
export function activeSection(tops: readonly number[], line: number, atEnd: boolean): number {
  if (tops.length === 0) return -1;
  if (atEnd) return tops.length - 1;
  let hit = 0;
  tops.forEach((top, i) => {
    if (top <= line) hit = i;
  });
  return hit;
}

/**
 * The scrollLeft that brings a tab of `width` at `left` fully inside a strip
 * showing `view` px from `scroll`, moving as little as possible. `pad` keeps
 * the tab clear of the strip's faded edge rather than parked under it.
 */
export function revealLeft(scroll: number, view: number, left: number, width: number, pad = 24): number {
  if (left - pad < scroll) return Math.max(0, left - pad);
  if (left + width + pad > scroll + view) return left + width + pad - view;
  return scroll;
}

/**
 * Where the document scrolls to put section `index` just under the pinned bar:
 * its top border lands behind the bar's own hairline, so the seam is one line,
 * not two. The first group goes to the very top instead, which also brings the
 * heading -- and on a phone the way back to the console -- into view with it.
 */
export function jumpTop(index: number, docTop: number, barHeight: number): number {
  return index === 0 ? 0 : Math.max(0, Math.round(docTop - barHeight + 1));
}

/* ------------------------------------------------------------------- the DOM */

/** How far below the bar a section's top may be and still count as arrived. */
const LEAD_PX = 48;

let current = -1;
/**
 * A tapped tab keeps its highlight until a hand touches the page again. A jump
 * to a group near the end bottoms the page out, where the scroll position on
 * its own would credit the LAST group instead of the one asked for; holding
 * also stops the highlight strobing through every group a smooth scroll passes.
 */
let held = -1;

const motion = (): ScrollBehavior =>
  window.matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth';

/**
 * Bring the bar in step with `groups`. Rebuilt only when the titles change:
 * settings repaints on every state frame, and recreating the tabs each second
 * would reset the strip's scroll and drop focus from a tab being keyboarded.
 * The comparison is against the tabs actually in the bar, not a remembered
 * copy, so the bar can never disagree with what it shows.
 */
export function paintJump(groups: readonly Group[]): void {
  const bar = $('setjump');
  const want = groups.map((g) => g.title).join('\n');
  const have = Array.from(bar.children, (tab) => tab.textContent).join('\n');
  if (want !== have) {
    // A held index or a lit index into the old list means nothing in the new one.
    current = -1;
    held = -1;
    bar.replaceChildren(
      ...groups.map((g, i) => {
        const tab = el('button', { type: 'button', textContent: g.title, onclick: () => jump(i) });
        tab.setAttribute('aria-controls', sectionId(g.title));
        return tab;
      }),
    );
    markOverflow(bar);
  }
  spy();
}

function jump(index: number): void {
  const section = $('groups').children[index];
  if (!section) return;
  held = index;
  mark(index);
  const docTop = section.getBoundingClientRect().top + window.scrollY;
  window.scrollTo({ top: jumpTop(index, docTop, $('setjump').offsetHeight), behavior: motion() });
}

/** Jump to the group titled `title` as its tab would: the header's UPDATE pill. */
export function jumpToSection(title: string): void {
  const i = Array.from($('setjump').children).findIndex((tab) => tab.textContent === title);
  if (i >= 0) jump(i);
}

function spy(): void {
  if ($('settings').classList.contains('hide')) return;
  if (held >= 0) {
    mark(held);
    return;
  }
  const line = $('setjump').getBoundingClientRect().bottom + LEAD_PX;
  const tops = Array.from($('groups').children, (g) => g.getBoundingClientRect().top);
  const doc = document.documentElement;
  const atEnd = window.scrollY > 0 && window.scrollY + window.innerHeight >= doc.scrollHeight - 2;
  mark(activeSection(tops, line, atEnd));
}

function mark(index: number): void {
  if (index === current) return;
  current = index;
  const bar = $('setjump');
  Array.from(bar.children).forEach((tab, i) => {
    if (i === index) tab.setAttribute('aria-current', 'location');
    else tab.removeAttribute('aria-current');
  });
  // On a phone the strip scrolls sideways; the lit tab is no use off its edge.
  const tab = bar.children[index];
  if (tab instanceof HTMLElement && bar.scrollWidth > bar.clientWidth) {
    const left = revealLeft(bar.scrollLeft, bar.clientWidth, tab.offsetLeft, tab.offsetWidth);
    bar.scrollTo({ left, behavior: motion() });
  }
}

/** Wire the listeners once, at boot. */
export function initJump(): void {
  const bar = $('setjump');
  let queued = false;
  const onScroll = (): void => {
    if (queued) return;
    queued = true;
    requestAnimationFrame(() => {
      queued = false;
      spy();
    });
  };
  window.addEventListener('scroll', onScroll, { passive: true });
  window.addEventListener('resize', () => {
    markOverflow(bar);
    onScroll();
  });
  bar.addEventListener('scroll', () => markOverflow(bar), { passive: true });
  // A hand on the page releases a held tab, and the scroll position takes over
  // again. Capture phase, so the release runs BEFORE the click of a tab that
  // is setting a new hold. Nothing is exempt -- not even the bar itself: a
  // wheel over it scrolls the page, and so do the arrow keys on a focused tab.
  const release = (): void => {
    held = -1;
  };
  for (const type of ['wheel', 'touchstart', 'pointerdown', 'keydown']) {
    window.addEventListener(type, release, { passive: true, capture: true });
  }
}
