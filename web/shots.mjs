#!/usr/bin/env node
/**
 * Phone-sized screenshot sweep of the console, for looking at it rather than
 * asserting on it.
 *
 *   npm run build && node shots.mjs round3 [--port 8123]
 *
 * The argument names a sweep, not a path: every sweep lands in the gitignored
 * <repo>/.shots/<name>/, so a run can never write outside the working copy.
 *
 * The Playwright suite already runs every spec in a `mobile` project, but an
 * assertion only fails on what someone thought to assert. Overflow, dead
 * vertical space, 20px tap targets and a paragraph that pushes the primary
 * control below the fold all pass a green suite -- they are only visible in a
 * picture. This writes that picture for both ends of the phone range, in the
 * states worth judging: the fold, the whole page, a commanded speed, a stopped
 * fan, the scope, the charts, a scrub in progress, the footer, a tip, settings,
 * and an unreachable device.
 *
 * Starts its own scripts/mock_device.py on its own port, like e2e/harness.ts
 * and for the same reason -- the mock keeps state in module globals, so
 * sharing one with a test run lets each corrupt the other's screen.
 *
 * Renders web/dist/console.html, so `npm run build` first or the sweep shows
 * the previous bundle.
 */
import { spawn } from 'node:child_process';
import { mkdirSync, readdirSync, unlinkSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium, devices } from '@playwright/test';

const REPO = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const ROOT = resolve(REPO, '.shots');

/**
 * Resolve the sweep name to a directory, or refuse.
 *
 * The argument is a NAME, never a path, and the result is checked to be inside
 * <repo>/.shots after resolution -- so `..`, an absolute path, or a symlinked
 * component cannot walk out. The first version took the directory straight
 * from argv and ran `rmSync(dir, {recursive:true, force:true})` on it, which
 * turns `node shots.mjs /` (or any typo) into a recursive delete of whatever
 * was named; SonarCloud flagged the argv-to-filesystem path twice and was
 * right both times. Nothing this script writes or deletes can now land outside
 * one gitignored directory in the working copy.
 */
function sweepDir(name) {
  if (!/^[A-Za-z0-9._-]+$/.test(name)) {
    throw new Error(`sweep name must be [A-Za-z0-9._-]+, got ${JSON.stringify(name)}`);
  }
  const dir = resolve(ROOT, name);
  if (dir !== ROOT && !dir.startsWith(`${ROOT}/`)) {
    throw new Error(`refusing to write outside ${ROOT}`);
  }
  return dir;
}

const outdir = sweepDir(process.argv[2] || 'latest');
const portArg = process.argv.indexOf('--port');
const PORT = portArg > -1 ? Number(process.argv[portArg + 1]) : 8123;
const BASE = `http://127.0.0.1:${PORT}`;

// Clear the previous sweep by removing only the PNGs this script writes, one
// level deep -- not by deleting the directory.
mkdirSync(outdir, { recursive: true });
for (const entry of readdirSync(outdir, { withFileTypes: true })) {
  if (entry.isFile() && entry.name.endsWith('.png')) unlinkSync(resolve(outdir, entry.name));
}

const mock = spawn('/usr/bin/python3', [`${REPO}/scripts/mock_device.py`], {
  env: { ...process.env, MOCK_PORT: String(PORT) },
  stdio: ['ignore', 'pipe', 'pipe'],
});
mock.stderr.on('data', () => {});

async function waitForMock() {
  const deadline = Date.now() + 30000;
  while (Date.now() < deadline) {
    try {
      const r = await fetch(`${BASE}/api/state`);
      if (r.ok) return;
    } catch {}
    await new Promise((r) => setTimeout(r, 100));
  }
  throw new Error('mock never came up');
}

// Both ends of the phone range. The iPhone SE preset is 320x568 -- the
// narrowest screen still in use, and the one where anything cramped fails
// first; Pixel 7 (412x839) is the ordinary Android case, where a layout can
// look finished while still being unusable at 320.
const VIEWPORTS = [
  { name: 'se', ...devices['iPhone SE'] },
  { name: 'pixel', ...devices['Pixel 7'] },
];

async function shoot(page, name, opts = {}) {
  await page.waitForTimeout(250);
  await page.screenshot({ path: `${outdir}/${name}.png`, ...opts });
  console.log(`  ${name}.png`);
}

/** Drag across a plot and hold, so the shot catches a scrub in progress. */
async function scrubShot(page, name, from, to) {
  const box = await page.locator('#cv_t').boundingBox();
  if (!box) return;
  const y = box.y + box.height / 2;
  await page.mouse.move(box.x + box.width * from, y);
  await page.mouse.down();
  await page.mouse.move(box.x + box.width * to, y, { steps: 8 });
  await page.waitForTimeout(300);
  await shoot(page, name);
  await page.mouse.up();
}

/** The hero, the controls, and both control states. */
async function shootConsole(page, name, width) {
  // The header alone, big enough to judge tap targets.
  await shoot(page, `${name}-00-header`, { clip: { x: 0, y: 0, width, height: 70 } });
  await shoot(page, `${name}-01-fold`);
  await shoot(page, `${name}-02-full`, { fullPage: true });

  await page.locator('#stack button[title="set speed 7"]').click();
  await page.waitForTimeout(400);
  await shoot(page, `${name}-03-speed7`);

  await page.locator('#boff').click();
  await page.waitForTimeout(400);
  await shoot(page, `${name}-04-off`);
  await page.locator('#stack button[title="set speed 4"]').click();
  await page.waitForTimeout(300);

  await page.locator('#pwmcell').click();
  await page.waitForTimeout(600);
  await shoot(page, `${name}-05-scope`, { fullPage: true });
  await page.locator('#scclose').click();
  await page.waitForTimeout(200);
}

/**
 * The charts, at 24 h and at the long ranges.
 *
 * A wide window is not the 24 h chart with different numbers on it: the axis
 * carries dates, the row step is hours rather than minutes, and gap detection
 * has to survive both -- which is exactly where "7D shows no data" lived. Each
 * range gets a plain shot and a scrubbed one, since the readout gains a date
 * there and that is the part with no room to spare at 320.
 */
async function shootCharts(page, name) {
  for (const label of ['HUM', 'FAN', 'PWR']) {
    const chip = page.locator('#chips button', { hasText: label });
    if (await chip.count()) await chip.first().click();
  }
  await page.waitForTimeout(500);
  await page.locator('#charts').scrollIntoViewIfNeeded();
  await page.waitForTimeout(300);
  await shoot(page, `${name}-06-charts`);
  await scrubShot(page, `${name}-07-scrub`, 0.35, 0.62);

  for (const days of [7, 30]) {
    const range = page.locator(`#ranges button[data-d="${days}"]`);
    if (!(await range.count())) continue;
    await range.click();
    await page.waitForTimeout(1200);
    await page.locator('#charts').scrollIntoViewIfNeeded();
    await page.waitForTimeout(300);
    await shoot(page, `${name}-07b-range${days}d`);
    await scrubShot(page, `${name}-07c-scrub${days}d`, 0.4, 0.55);
  }
  await page.locator('#ranges button[data-d="1"]').click();
  await page.waitForTimeout(800);
}

/** The footer, a tooltip, and the settings screen. */
async function shootFooterAndSettings(page, name) {
  await page.locator('#stats').scrollIntoViewIfNeeded();
  await page.waitForTimeout(300);
  await shoot(page, `${name}-08-bottom`);

  /*
   * TAP, not click. The bits have toggled their tip on click for a long time,
   * and `.click()` moves the pointer in first -- which OPENS the tip on hover,
   * so the click that follows closes it again and the shot comes out empty.
   * Three review rounds read that empty frame as "tooltips never appear on
   * touch" and filed it as a product bug; it was this line. A tap dispatches
   * touch without a preceding hover, which is what a phone actually does.
   */
  const bit = page.locator('#stats span.bit').first();
  if (await bit.count()) {
    await bit.tap().catch(() => bit.dispatchEvent('click'));
    await page.waitForTimeout(300);
    /*
     * fullPage, because #tip is anchored to the BOTTOM of #statwrap and grows
     * upward. Scrolling the status strip into view therefore puts the tip
     * itself above the frame, and a viewport shot of it is blank whether the
     * tip opened or not -- which is the other half of why three rounds read
     * this as "tooltips never appear on touch".
     */
    await shoot(page, `${name}-09-tip`, { fullPage: true });
  }

  // Clicked through the DOM: if the header overflows on a narrow phone the
  // button can be off-screen or covered -- that is a bug to see in the shots,
  // not a reason for the run to die here.
  await page.evaluate(() => document.getElementById('nav')?.click());
  await page.waitForTimeout(600);
  await shoot(page, `${name}-10-settings`);
  await shoot(page, `${name}-11-settings-full`, { fullPage: true });
}

async function run() {
  await waitForMock();
  const browser = await chromium.launch();
  for (const vp of VIEWPORTS) {
    const { name, ...device } = vp;
    const ctx = await browser.newContext({ ...device, baseURL: BASE });
    const page = await ctx.newPage();
    console.log(`[${name}] ${device.viewport.width}x${device.viewport.height}`);

    await page.goto('/');
    await page.waitForSelector('#railnum:not(:has-text("–"))', { timeout: 15000 });
    await page.waitForTimeout(700);

    await shootConsole(page, name, device.viewport.width);
    await shootCharts(page, name);
    await shootFooterAndSettings(page, name);

    await ctx.close();
  }

  /**
   * Offline / degraded, on the small phone only.
   *
   * Waits past app.ts's OFFLINE_AFTER_MS (45 s) rather than a few seconds. The
   * first version waited 6 s and photographed a page that had not yet decided
   * it was offline, so the shot showed a fully live-looking screen and the
   * review recorded "the offline state is identical to the live state" without
   * being able to tell a missing state from an unphotographed one. A sweep that
   * cannot distinguish those two is worse than no sweep: it produces confident
   * findings about code it never reached. It also asserts the state actually
   * arrived, so a future regression fails here loudly instead of quietly
   * yielding a screenshot of the wrong thing.
   */
  const ctx = await browser.newContext({ ...devices['iPhone SE'], baseURL: BASE });
  const page = await ctx.newPage();
  await page.goto('/');
  await page.waitForSelector('#railnum:not(:has-text("–"))', { timeout: 15000 });
  await page.waitForTimeout(800);
  await fetch(`${BASE}/_scen?down=true`);
  try {
    await page.waitForFunction(() => document.body.classList.contains('offline'), null, {
      timeout: 70000,
    });
  } catch {
    console.log('  !! the page never entered the offline state -- shooting it anyway');
  }
  await shoot(page, 'se-12-offline', { fullPage: true });
  await shoot(page, 'se-13-offline-header', { clip: { x: 0, y: 0, width: 320, height: 70 } });
  await ctx.close();

  /*
   * COLD: the device is already dead when the page loads.
   *
   * A different path from the warm one and the more common one in a garage --
   * you walk in, open the console, and the fan has been down for hours, so
   * there is no last-known anything to decay from. It was a real bug (the
   * offline guard measured from a lastOk that is 0 until a poll succeeds) and
   * the sweep could not show it either way, because it always loaded against a
   * live mock first.
   */
  const coldCtx = await browser.newContext({ ...devices['iPhone SE'], baseURL: BASE });
  const cold = await coldCtx.newPage();
  await cold.goto('/');
  try {
    await cold.waitForFunction(() => document.body.classList.contains('offline'), null, {
      timeout: 70000,
    });
  } catch {
    console.log('  !! the cold page never entered the offline state -- shooting it anyway');
  }
  await shoot(cold, 'se-14-offline-cold', { fullPage: true });
  await coldCtx.close();
  // Reset the knob from NODE, not through a page: the contexts that set it are
  // closed by now, and `page.request` on a closed context fails silently -- which
  // left `down=true` set and made every later shot a picture of a dead device.
  await fetch(`${BASE}/_scen?down=false`);

  /*
   * The sticky chart header, photographed while it is actually doing its job.
   * Every other chart shot has #chead near the top of the viewport anyway, so
   * they cannot show whether it pins -- three rounds of review could only note
   * that the claim was unverified. Scroll the plot's middle to the top of the
   * screen and the header is either still there or it is not.
   */
  const stickyCtx = await browser.newContext({ ...devices['iPhone SE'], baseURL: BASE });
  const sticky = await stickyCtx.newPage();
  await sticky.goto('/');
  await sticky.waitForSelector('#railnum:not(:has-text("–"))', { timeout: 15000 });
  await sticky.waitForTimeout(900);
  await sticky.evaluate(() => {
    const cv = document.getElementById('cv_t');
    if (cv) {
      const mid = cv.getBoundingClientRect().top + scrollY + cv.getBoundingClientRect().height / 2;
      window.scrollTo(0, mid);
    }
  });
  await sticky.waitForTimeout(400);
  await shoot(sticky, 'se-15-charts-scrolled');
  await stickyCtx.close();

  await browser.close();
}

try {
  await run();
} finally {
  mock.kill('SIGTERM');
}
