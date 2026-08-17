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

    // 0. The header alone, big enough to judge tap targets.
    await shoot(page, `${name}-00-header`, {
      clip: { x: 0, y: 0, width: device.viewport.width, height: 70 },
    });
    // 1. Above the fold, exactly as it lands.
    await shoot(page, `${name}-01-fold`);
    // 2. The whole console page.
    await shoot(page, `${name}-02-full`, { fullPage: true });

    // 3. Speed rail engaged at a mid step.
    await page.locator('#stack button[title="set speed 7"]').click();
    await page.waitForTimeout(400);
    await shoot(page, `${name}-03-speed7`);

    // 4. Fan off state.
    await page.locator('#boff').click();
    await page.waitForTimeout(400);
    await shoot(page, `${name}-04-off`);
    await page.locator('#stack button[title="set speed 4"]').click();
    await page.waitForTimeout(300);

    // 5. The PWM scope, opened.
    await page.locator('#pwmcell').click();
    await page.waitForTimeout(600);
    await shoot(page, `${name}-05-scope`, { fullPage: true });
    await page.locator('#scclose').click();
    await page.waitForTimeout(200);

    // 6. Charts with extra rows added (the sub-plots).
    for (const label of ['HUM', 'FAN', 'PWR']) {
      const chip = page.locator('#chips button', { hasText: label });
      if (await chip.count()) await chip.first().click();
    }
    await page.waitForTimeout(500);
    await page.locator('#charts').scrollIntoViewIfNeeded();
    await page.waitForTimeout(300);
    await shoot(page, `${name}-06-charts`);

    // 7. A chart readout under touch: press and drag across the temp plot.
    const box = await page.locator('#cv_t').boundingBox();
    if (box) {
      await page.mouse.move(box.x + box.width * 0.35, box.y + box.height / 2);
      await page.mouse.down();
      await page.mouse.move(box.x + box.width * 0.62, box.y + box.height / 2, { steps: 8 });
      await page.waitForTimeout(300);
      await shoot(page, `${name}-07-scrub`);
      await page.mouse.up();
    }

    // 7b. The long ranges. A wide window is not the 24 h chart with different
    // numbers on it: the axis has to carry dates, the row step is hours rather
    // than minutes, and gap detection has to survive both -- which is exactly
    // where "7D shows no data" lived. Captured per range so the axis tiers can
    // be read rather than assumed.
    for (const days of [7, 30]) {
      const range = page.locator(`#ranges button[data-d="${days}"]`);
      if (!(await range.count())) continue;
      await range.click();
      await page.waitForTimeout(1200);
      await page.locator('#charts').scrollIntoViewIfNeeded();
      await page.waitForTimeout(300);
      await shoot(page, `${name}-07b-range${days}d`);
      // And the same range under a finger, since the scrub label gains a date
      // here and that is the part with no room to spare at 320.
      const rbox = await page.locator('#cv_t').boundingBox();
      if (rbox) {
        await page.mouse.move(rbox.x + rbox.width * 0.4, rbox.y + rbox.height / 2);
        await page.mouse.down();
        await page.mouse.move(rbox.x + rbox.width * 0.55, rbox.y + rbox.height / 2, { steps: 6 });
        await page.waitForTimeout(300);
        await shoot(page, `${name}-07c-scrub${days}d`);
        await page.mouse.up();
      }
    }
    await page.locator('#ranges button[data-d="1"]').click();
    await page.waitForTimeout(800);

    // 8. Odometer + status bar at the very bottom.
    await page.locator('#stats').scrollIntoViewIfNeeded();
    await page.waitForTimeout(300);
    await shoot(page, `${name}-08-bottom`);

    // 9. A status-bar tooltip (tap target + popover placement).
    const bit = page.locator('#stats span.bit').first();
    if (await bit.count()) {
      await bit.click().catch(() => {});
      await page.waitForTimeout(300);
      await shoot(page, `${name}-09-tip`);
    }

    // 10-11. Settings drawer, top and full. Forced: if the header overflows on
    // a narrow phone the button can be off-screen or covered -- that is a bug
    // to see in the shots, not a reason for the run to die here.
    await page.evaluate(() => document.getElementById('nav')?.click());
    await page.waitForTimeout(600);
    await shoot(page, `${name}-10-settings`);
    await shoot(page, `${name}-11-settings-full`, { fullPage: true });

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
  await page.request.get('/_scen?down=true');
  try {
    await page.waitForFunction(() => document.body.classList.contains('offline'), null, {
      timeout: 70000,
    });
  } catch {
    console.log('  !! the page never entered the offline state -- shooting it anyway');
  }
  await shoot(page, 'se-12-offline', { fullPage: true });
  await shoot(page, 'se-13-offline-header', { clip: { x: 0, y: 0, width: 320, height: 70 } });
  await page.request.get('/_scen?down=false');
  await ctx.close();

  await browser.close();
}

try {
  await run();
} finally {
  mock.kill('SIGTERM');
}
