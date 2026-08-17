import { execFileSync, spawn, type ChildProcess } from 'node:child_process';
import { existsSync } from 'node:fs';
import { expect, test as base, type CDPSession, type Page, type Request } from '@playwright/test';

/**
 * Shared harness for the console's end-to-end pass.
 *
 * Three things every spec needs and none should hand-roll:
 *
 *  - ONE MOCK PER WORKER. mock_device.py keeps STATE and SCEN in module
 *    globals, and the console mutates STATE on nearly every interaction:
 *    /api/set from the speed rail, /api/config from the drawer's steppers and
 *    toggles. A single shared mock therefore lets one test's write land between
 *    another test's action and its assertion. That is not theoretical -- the
 *    `down=true` knob took the whole suite down with it before this existed.
 *    A Playwright worker runs one test at a time, so a mock per worker is
 *    complete isolation, and it retires the special-case second server that
 *    scenarios.spec.ts used to need.
 *  - Scenario knobs reset between tests, for the specs that flip them.
 *  - A console-error guard. Most console regressions surface first as an
 *    uncaught TypeError with the page still looking plausible, so a spec that
 *    only asserts on visible text passes while the app is broken underneath.
 */

/** First port of the per-worker block; worker N listens on BASE + N. */
const MOCK_PORT_BASE = Number(process.env['MOCK_PORT'] ?? 8100);

/**
 * The interpreter to run the mock with, as an ABSOLUTE path.
 *
 * Spawning bare `python3` resolves through PATH, so whatever comes first there
 * gets executed -- which is why static analysis flags it and why it is worth
 * not doing even in a test harness. Resolved once per worker: the common fixed
 * locations first, then, for CI where setup-python installs into a tool cache
 * outside them, asking Python itself for sys.executable via /usr/bin/env (an
 * absolute path in its own right).
 */
function pythonPath(): string {
  const explicit = process.env['MOCK_PYTHON'];
  if (explicit) return explicit;
  for (const candidate of [
    '/usr/bin/python3',
    '/usr/local/bin/python3',
    '/opt/homebrew/bin/python3',
  ]) {
    if (existsSync(candidate)) return candidate;
  }
  return execFileSync('/usr/bin/env', ['python3', '-c', 'import sys;print(sys.executable)'])
    .toString()
    .trim();
}

const PYTHON = pythonPath();

async function waitForMock(port: number, proc: ChildProcess): Promise<void> {
  const deadline = Date.now() + 30_000;
  while (Date.now() < deadline) {
    if (proc.exitCode !== null) {
      throw new Error(`mock_device.py exited with ${proc.exitCode} before serving :${port}`);
    }
    try {
      const res = await fetch(`http://127.0.0.1:${port}/api/state`);
      if (res.ok) return;
    } catch {
      /* not listening yet */
    }
    await new Promise((r) => setTimeout(r, 100));
  }
  throw new Error(`mock_device.py never answered on :${port}`);
}

export const SCEN_DEFAULTS = {
  card: 'true',
  synced: 'true',
  // Rows the mock card HOLDS -- 60 days at 300 s, i.e. enough to fill every
  // range. Must track mock_state.SCEN or a reset would leave the mock in a
  // state its own default never produces.
  rows: '17280',
  gap_at: 'none',
  corrupt: 'false',
  flat_rh: 'false',
  down: 'false',
  // EVERY knob in mock_state.SCEN_SPEC belongs here, including the ones no
  // spec in this file flips today. A reset is only as good as its
  // most-forgotten key: `plug` was missing, so the two plug-disagreement
  // specs left the meter reading 43.5 W for whatever the worker ran next, and
  // "the DRAW cell shows the measured watts" failed on 20.3 vs 43.5 whenever
  // the order happened to put it second. It survived on luck for as long as
  // the ordering held, which is the worst way for a test to pass.
  panel_ready: 'true',
  plug: 'ok',
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
export const test = base.extend<{ errors: string[] }, { mockPort: number }>({
  // Worker-scoped: one mock process per worker, started once and reused by
  // every test that worker runs, torn down when the worker exits.
  mockPort: [
    async ({}, use, workerInfo) => {
      const port = MOCK_PORT_BASE + workerInfo.parallelIndex;
      const proc = spawn(PYTHON, ['../scripts/mock_device.py'], {
        env: { ...process.env, MOCK_PORT: String(port) },
        stdio: ['ignore', 'pipe', 'pipe'],
      });
      try {
        await waitForMock(port, proc);
        await use(port);
      } finally {
        proc.kill('SIGTERM');
      }
    },
    { scope: 'worker' },
  ],

  // Point every test at its own worker's mock.
  baseURL: async ({ mockPort }, use) => {
    await use(`http://127.0.0.1:${mockPort}`);
  },

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

/**
 * How many pixel columns of a canvas have anything drawn in them.
 *
 * The charts are hand-rolled canvas, so "did it actually plot something" is not
 * visible in the DOM at all -- a test that checks a caption passes just as
 * happily when the plot area is empty. Measured: a normal 24 h render inks
 * ~1097 of 1136 columns, and a failed history load inks exactly 0, which makes
 * blank-vs-drawn an unambiguous assertion.
 *
 * It does NOT resolve whether an outage is drawn as a break in the line: the
 * shaded band between garage and yard inks those columns either way, so a
 * bridged line and a real gap look the same from here (measured: 1100 inked
 * columns with a gap, 1097 without). Distinguishing them needs the series state
 * exposed to the page or a real visual-regression pass; see scenarios.spec.ts.
 */
export async function inkedColumns(page: Page, canvasId: string): Promise<number> {
  return page.evaluate((id) => {
    const c = document.getElementById(id) as HTMLCanvasElement | null;
    if (!c) return -1;
    const ctx = c.getContext('2d');
    if (!ctx) return -1;
    const { width: w, height: h } = c;
    if (w === 0 || h === 0) return 0;
    const data = ctx.getImageData(0, 0, w, h).data;
    let inked = 0;
    for (let x = 0; x < w; x++) {
      for (let y = 0; y < h; y++) {
        if ((data[(y * w + x) * 4 + 3] ?? 0) > 8) {
          inked++;
          break;
        }
      }
    }
    return inked;
  }, canvasId);
}

export interface Point {
  x: number;
  y: number;
}

/**
 * A real finger drag, through the browser's own touch pipeline.
 *
 * Playwright's touchscreen API only taps, and page.mouse never enters the touch
 * path at all -- so the gesture where the console has to CHOOSE between
 * scrubbing the chart and letting the page scroll was untestable, which is how
 * a touchmove handler that called preventDefault() on every move shipped and
 * turned 400 px of chart into a region a phone could not scroll out of.
 *
 * CDP dispatches what the compositor itself sees, so the scrolling the browser
 * does on our behalf is part of what gets asserted. Chromium-only, which both
 * projects in playwright.config.ts are.
 *
 * `hold` leaves the finger DOWN, because that is the only moment a scrub is
 * observable: releasing ends it by design.
 */
// One session per page, kept: a finger held down belongs to the session that
// pressed it, and a fresh session refuses the release with "Must send a
// TouchStart first" -- which is how the held-scrub test failed the first time.
const cdpSessions = new WeakMap<Page, CDPSession>();

async function touchSession(page: Page): Promise<CDPSession> {
  const existing = cdpSessions.get(page);
  if (existing) return existing;
  const fresh = await page.context().newCDPSession(page);
  cdpSessions.set(page, fresh);
  return fresh;
}

export async function touchDrag(
  page: Page,
  from: Point,
  to: Point,
  { hold = false, steps = 8 }: { hold?: boolean; steps?: number } = {},
): Promise<void> {
  const cdp = await touchSession(page);
  const at = (
    type: 'touchStart' | 'touchMove' | 'touchEnd',
    x: number,
    y: number,
  ): Promise<unknown> =>
    cdp.send('Input.dispatchTouchEvent', {
      type,
      touchPoints: type === 'touchEnd' ? [] : [{ x, y }],
    });
  await at('touchStart', from.x, from.y);
  for (let i = 1; i <= steps; i++) {
    await at(
      'touchMove',
      from.x + ((to.x - from.x) * i) / steps,
      from.y + ((to.y - from.y) * i) / steps,
    );
  }
  if (!hold) await at('touchEnd', to.x, to.y);
}

/** Lift a finger left down by `touchDrag(..., { hold: true })`. */
export async function touchRelease(page: Page): Promise<void> {
  const cdp = await touchSession(page);
  await cdp.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
}

/** True on the phone project; the desk project has no touchscreen to drive. */
export async function hasTouch(page: Page): Promise<boolean> {
  return page.evaluate(() => 'ontouchstart' in window);
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
