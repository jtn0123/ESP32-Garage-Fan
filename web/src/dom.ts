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
