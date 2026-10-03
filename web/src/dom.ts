// Minimal DOM helpers. No framework: the console has a handful of stateful
// values and one paint() that re-derives from them, which is the shape a
// virtual DOM would give us anyway -- without adding a runtime to a page that
// has to fit in the flash left over after the firmware.

/** Look up an element that the HTML shell is known to contain. */
export function $<T extends HTMLElement = HTMLElement>(id: string): T {
  const el = document.getElementById(id);
  if (!el) throw new Error(`missing element #${id}`);
  return el as T;
}

/** Look up an element that may legitimately be absent (settings rows come and go). */
export function maybe<T extends HTMLElement = HTMLElement>(id: string): T | null {
  return document.getElementById(id) as T | null;
}

export function el<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  props: Partial<HTMLElementTagNameMap[K]> = {},
): HTMLElementTagNameMap[K] {
  return Object.assign(document.createElement(tag), props);
}

export function show(target: HTMLElement, visible: boolean): void {
  target.classList.toggle('hide', !visible);
}

export function clear(target: HTMLElement): void {
  target.replaceChildren();
}

/**
 * Does a sideways-scrolling strip continue past its right edge?
 *
 * The `more` class drives the edge fade. Kept in sync from the scroll position
 * rather than assumed from the width, because the answer changes as you scroll
 * and a fade that stays put once you have reached the last chip is a lie about
 * there being more. Shared by the chart's row chips and the settings jump bar.
 */
export function markOverflow(host: HTMLElement): void {
  const more = host.scrollLeft + host.clientWidth < host.scrollWidth - 2;
  host.classList.toggle('more', more);
}

/**
 * Array access that says what it means.
 *
 * tsconfig sets noUncheckedIndexedAccess, so `series[i]` is `T | undefined`.
 * Every chart read goes through a scrub index that can legitimately point past
 * the end of a shorter series, so the guard is real, not ceremony.
 */
export function at(series: readonly (number | null)[] | undefined, i: number): number | null {
  if (!series || i < 0 || i >= series.length) return null;
  const v = series[i];
  return v === undefined || v === null || Number.isNaN(v) ? null : v;
}

/**
 * How far to scroll so the span [top, bottom] (viewport px) is on screen.
 *
 * The least movement that does it, like `scrollIntoView({ block: 'nearest' })`
 * but as a number the caller can test and decide on: 0 when the span is
 * already in view, positive to scroll down to its foot, negative to scroll up
 * to its head. A span taller than the screen leads with its head -- the top of
 * a panel is where its title and its way out are.
 */
export function revealBy(top: number, bottom: number, viewH: number, pad = 12): number {
  if (bottom - top + 2 * pad > viewH) return top - pad;
  if (bottom > viewH - pad) return bottom - (viewH - pad);
  if (top < pad) return top - pad;
  return 0;
}
