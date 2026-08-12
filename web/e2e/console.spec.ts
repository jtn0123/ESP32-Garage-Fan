/**
 * The main screen: every control that is not behind the settings drawer.
 *
 * These run against the same mock concurrently, so nothing here may flip a
 * scenario knob -- knob-flipping lives in scenarios.spec.ts, which is serial.
 */
import { expect, openConsole, recordRequests, test } from './harness';

test('loads, paints live data, and reports no errors', async ({ page }) => {
  await openConsole(page);
  await expect(page.locator('#brand')).toHaveText('GARAGE FAN');
  await expect(page.locator('#railnum')).not.toHaveText('–');
  // "Connecting to the controller…" must not survive a successful connection.
  await expect(page.locator('#reason')).not.toContainText('Connecting');
});

test('the status bar fills in from /api/state', async ({ page }) => {
  await openConsole(page);
  await expect(page.locator('#hmq')).toContainText('BROKER');
  await expect(page.locator('#hfw')).not.toHaveText('FW —');
  await expect(page.locator('#hbat')).toBeVisible(); // mock reports a battery
});

test('the hero shows garage, outside and differential', async ({ page }) => {
  await openConsole(page);
  for (const id of ['#tT', '#tO', '#tD']) {
    await expect(page.locator(id)).not.toHaveText('–');
  }
});

// ------------------------------------------------------------- the speed rail

test('the rail offers every running speed', async ({ page }) => {
  await openConsole(page);
  // 1..12. Zero is deliberately NOT a rail step -- it is the "Fan off" pill,
  // so stopping the fan cannot happen by a stray click at the end of the rail.
  await expect(page.locator('#stack button')).toHaveCount(12);
  await expect(page.locator('#stack button[title="set speed 1"]')).toBeVisible();
  await expect(page.locator('#stack button[title="set speed 12"]')).toBeVisible();
  await expect(page.locator('#stack button[title="set speed 0"]')).toHaveCount(0);
});

test('clicking a rail step commands that speed', async ({ page }) => {
  await openConsole(page);
  const posts = recordRequests(page, /\/api\/set/);
  // Index 0 is the TOP of the rail (speed 12) and the last is speed 0, so
  // assert on the title rather than position -- a reordered rail must not
  // silently start sending the wrong speed.
  await page.locator('#stack button[title="set speed 7"]').click();
  await expect(page.locator('#railnum')).toHaveText('7');
  expect(posts.length).toBeGreaterThan(0);
  expect(posts.some((r) => /speed=7/.test(r.url()))).toBeTruthy();
});

test('both ends of the rail work', async ({ page }) => {
  await openConsole(page);
  await page.locator('#stack button[title="set speed 12"]').click();
  await expect(page.locator('#railnum')).toHaveText('12');
  await page.locator('#stack button[title="set speed 1"]').click();
  await expect(page.locator('#railnum')).toHaveText('1');
});

test('"Fan off" stops the fan and reads as off, not as a speed', async ({ page }) => {
  await openConsole(page);
  const posts = recordRequests(page, /\/api\/set/);
  await page.locator('#stack button[title="set speed 9"]').click();
  await expect(page.locator('#railnum')).toHaveText('9');
  await page.locator('#boff').click();
  // The readout says "off" rather than "0": a stopped fan should not look like
  // just another step on the dial.
  await expect(page.locator('#railnum')).toHaveText('off');
  await expect.poll(() => posts.some((r) => /speed=0/.test(r.url()))).toBeTruthy();
});

test('the auto pill toggles auto mode', async ({ page }) => {
  await openConsole(page);
  const posts = recordRequests(page, /\/api\/(set|config)/);
  const before = await page.locator('#bauto').getAttribute('class');
  await page.locator('#bauto').click();
  await expect
    .poll(() => page.locator('#bauto').getAttribute('class'))
    .not.toBe(before);
  expect(posts.length).toBeGreaterThan(0);
});

// ------------------------------------------------------------------ the scope

test('the PWM cell opens the waveform scope and closes again', async ({ page }) => {
  await openConsole(page);
  const scope = page.locator('#scope');
  await expect(scope).toHaveClass(/hide/);
  await page.locator('#pwmcell').click();
  await expect(scope).not.toHaveClass(/hide/);
  await expect(page.locator('#cv_pw')).toBeVisible();
  await page.locator('#scclose').click();
  await expect(scope).toHaveClass(/hide/);
});

test('the capture table offers all thirteen steps and transmits one', async ({ page }) => {
  await openConsole(page);
  await page.locator('#pwmcell').click();
  const cells = page.locator('#capgrid > *');
  await expect(cells).toHaveCount(13);
  const posts = recordRequests(page, /\/api\/set/);
  await cells.nth(4).click();
  await expect.poll(() => posts.length).toBeGreaterThan(0);
});

// ------------------------------------------------------------------ the drawer

test('SETTINGS opens the drawer and back returns to the console', async ({ page }) => {
  await openConsole(page);
  await expect(page.locator('#settings')).toHaveClass(/hide/);
  await page.locator('#nav').click();
  await expect(page.locator('#settings')).not.toHaveClass(/hide/);
  await expect(page.locator('#console')).toHaveClass(/hide/);
  await page.locator('#nav').click();
  await expect(page.locator('#settings')).toHaveClass(/hide/);
  await expect(page.locator('#console')).not.toHaveClass(/hide/);
});

// ------------------------------------------------------------------- fallbacks

/**
 * The device pushes state over SSE on port 8081; the mock does not serve it, so
 * EventSource fails here exactly as it does when that listener is down. The
 * page must still track the fan off the 15 s poll -- api.ts swallows the
 * EventSource error precisely so this keeps working, and nothing tested that.
 */
test('state still updates when the SSE stream is unavailable', async ({ page }) => {
  // Attached BEFORE navigation: the state fetch that proves the fallback works
  // happens during load, so a recorder installed afterwards sees nothing until
  // the 15 s poll comes round and the test would be asserting on its own timing.
  const polls = recordRequests(page, /\/api\/state/);
  await openConsole(page);
  expect(polls.length, 'the console never fetched state over plain HTTP').toBeGreaterThan(0);
  // And the page is live, not merely painted once.
  await page.locator('#stack button[title="set speed 3"]').click();
  await expect(page.locator('#railnum')).toHaveText('3');
});

test('the page never scrolls sideways', async ({ page }) => {
  await openConsole(page);
  const overflow = await page.evaluate(
    () => document.documentElement.scrollWidth - document.documentElement.clientWidth,
  );
  expect(overflow, 'the layout overflows horizontally').toBeLessThanOrEqual(1);
});
