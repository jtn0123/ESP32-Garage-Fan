import { expect, test as base, type Page, type Request } from '@playwright/test';

/**
 * Shared harness for the console's end-to-end pass.
 *
 * Two things every spec needs and neither should hand-roll:
 *
 *  - Scenario knobs reset between tests. mock_device.py holds SCEN in module
 *    state, so a spec that unmounts the card leaks that into whatever runs
 *    next. Tests run in parallel against ONE mock, so knob-flipping specs are
 *    marked serial and reset in an inner fixture rather than globally.
 *  - A console-error guard. Most console regressions surface first as an
 *    uncaught TypeError with the page still looking plausible, so a spec that
 *    only asserts on visible text passes while the app is broken underneath.
 */

export const SCEN_DEFAULTS = {
  card: 'true',
  synced: 'true',
  rows: '288',
  gap_at: 'none',
  corrupt: 'false',
  flat_rh: 'false',
  down: 'false',
} as const;

/** Flip scenario knobs on the mock. */
export async function scen(page: Page, knobs: Record<string, string | number>): Promise<void> {
  const qs = Object.entries(knobs)
    .map(([k, v]) => `${k}=${v}`)
    .join('&');
  const res = await page.request.get(`/_scen?${qs}`);
  expect(res.ok(), `/_scen?${qs} was refused`).toBeTruthy();
}

export async function resetScen(page: Page): Promise<void> {
  await scen(page, SCEN_DEFAULTS as unknown as Record<string, string>);
}

/**
 * The base fixture: fails a test if the page logged an error or threw.
 *
 * Deliberately not an afterEach in each spec -- attached here it cannot be
 * forgotten, which is the entire point of having it.
 */
export const test = base.extend<{ errors: string[] }>({
  errors: async ({ page }, use) => {
    const errors: string[] = [];
    page.on('console', (m) => {
      if (m.type() === 'error') errors.push(`console.error: ${m.text()}`);
    });
    page.on('pageerror', (e) => errors.push(`pageerror: ${e.message}`));
    await use(errors);
    // Failed fetches are the app's own business (offline scenarios expect
    // them); an uncaught exception never is.
    const fatal = errors.filter((e) => !/Failed to load resource/.test(e));
    expect(fatal, 'the page reported errors').toEqual([]);
  },
});

export { expect };

/**
 * Prove that anything the page was going to send has already been sent.
 *
 * Asserting a request did NOT happen is the awkward case: there is no event for
 * an absence, and `waitForTimeout(500)` is a guess that gets slower and flakier
 * on a loaded CI box. Instead, issue a fetch FROM THE PAGE and await it. A
 * request the click handler dispatched was created earlier in the same task
 * queue, and Playwright records a request when it is dispatched -- so once ours
 * completes, theirs is already in the log if it exists at all. Ordering, not
 * hope.
 */
export async function flushPageRequests(page: Page): Promise<void> {
  await page.evaluate(() => fetch('/api/state', { cache: 'no-store' }).then((r) => r.text()));
}

/** Record every request the console makes to a path, for "did it actually call?" */
export function recordRequests(page: Page, match: RegExp): Request[] {
  const seen: Request[] = [];
  page.on('request', (r) => {
    if (match.test(r.url())) seen.push(r);
  });
  return seen;
}

/**
 * Open the console and wait until it has painted real data.
 *
 * The temperature reading starts as an en dash and is replaced on the first
 * /api/state response, so it is the honest "the app is live" signal -- waiting
 * on load events instead lets assertions race the first paint.
 */
export async function openConsole(page: Page): Promise<void> {
  await page.goto('/');
  await expect(page.locator('#tT')).not.toHaveText('–', { timeout: 15_000 });
  // Boot fires several fetches (state, device, sensors, stats, history) and
  // each repaint calls replaceChildren on the status strip, which destroys the
  // node under the cursor. Interacting mid-burst is not what a person does, and
  // a test that does it is testing the burst rather than the control.
  //
  // NOTE: this papers over nothing -- the same replaceChildren drops a tooltip
  // the user is actively reading when the 15 s poll lands. That is a real, if
  // small, bug in console.ts::paintStats and it is recorded in the review notes
  // rather than retried away here.
  await expect(page.locator('#stats .bit').first()).toBeVisible({ timeout: 15_000 });
  await page.waitForLoadState('networkidle');
}
