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
  // Polled, not read once: the DOM can settle before the browser has emitted
  // the request, so an immediate read is a race that fails on a loaded machine.
  await expect
    .poll(() => posts.some((r) => new URL(r.url()).searchParams.get('speed') === '7'), {
      message: 'the rail updated the display without commanding the fan',
    })
    .toBeTruthy();
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
  // Parsed, not substring-matched: "speed=0" is also a prefix of "speed=07".
  await expect
    .poll(() => posts.some((r) => new URL(r.url()).searchParams.get('speed') === '0'))
    .toBeTruthy();
});

test('the auto pill toggles auto mode', async ({ page }) => {
  await openConsole(page);
  const posts = recordRequests(page, /\/api\/(set|config)/);
  const before = await page.locator('#bauto').getAttribute('class');
  await page.locator('#bauto').click();
  await expect
    .poll(() => page.locator('#bauto').getAttribute('class'))
    .not.toBe(before);
  // Same race as the rail: the pill can repaint before the request is emitted.
  await expect
    .poll(() => posts.length, { message: 'the pill changed but nothing was sent' })
    .toBeGreaterThan(0);
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
 * page must keep tracking the fan off the 15 s poll -- api.ts swallows the
 * EventSource error precisely so this keeps working.
 *
 * The assertion has to be about a poll AFTER bootstrap. Counting requests from
 * page load proves only that the page loaded: a build where the SSE failure
 * killed the polling loop outright would still make that request once and pass.
 * So: take the count once the page is up, then require it to grow. The wait is
 * longer than the 15 s interval, which is why this is the slowest test here.
 */
test('state keeps polling when the SSE stream is unavailable', async ({ page }) => {
  await openConsole(page);
  const polls = recordRequests(page, /\/api\/state/);
  const atBootstrap = polls.length;
  await expect
    .poll(() => polls.length, {
      message: 'no /api/state poll after bootstrap: the fallback is not running',
      timeout: 25_000,
    })
    .toBeGreaterThan(atBootstrap);
});

test('the page stays interactive without SSE', async ({ page }) => {
  await openConsole(page);
  await page.locator('#stack button[title="set speed 3"]').click();
  await expect(page.locator('#railnum')).toHaveText('3');
});

/**
 * The narrowest phone still in service (iPhone SE, 320 CSS px).
 *
 * The header was one `white-space:nowrap` row measuring ~394 px there, so it
 * overflowed: the document became wider than the screen and the page panned
 * sideways, and SETTINGS sat past the right edge where nothing could reach it.
 * From the settings screen the same button is the ONLY way back, so opening
 * the drawer -- if you managed to -- was a one-way trip.
 */
test('every header control is reachable at 320 px', async ({ page }) => {
  await page.setViewportSize({ width: 320, height: 568 });
  await openConsole(page);

  const onScreen = async (sel: string): Promise<boolean> =>
    page.evaluate((s) => {
      const r = document.querySelector(s)?.getBoundingClientRect();
      const w = document.documentElement.clientWidth;
      return !!r && r.left >= 0 && r.right <= w && r.width > 0;
    }, sel);

  expect(await onScreen('#nav'), 'SETTINGS is off screen at 320 px').toBeTruthy();
  expect(await onScreen('#brand')).toBeTruthy();
  expect(await onScreen('#hmq')).toBeTruthy();
  // clientWidth, not innerWidth: it is the layout width in both projects
  // (mobile emulation keeps reporting the device's own innerWidth after a
  // resize, and a desk browser's scrollbar is not part of the page either).
  const overflow = await page.evaluate(
    () => document.documentElement.scrollWidth - document.documentElement.clientWidth,
  );
  expect(overflow, 'the page pans sideways at 320 px').toBeLessThanOrEqual(1);

  // The way back has to be reachable too, or the drawer is a trap.
  await page.locator('#nav').click();
  await expect(page.locator('#settings')).not.toHaveClass(/hide/);
  expect(await onScreen('#nav'), 'the return control is off screen at 320 px').toBeTruthy();
  await expect(page.locator('#nav')).toHaveText(/CONSOLE/);
  await page.locator('#nav').click();
  await expect(page.locator('#console')).not.toHaveClass(/hide/);
});

test('the page never scrolls sideways', async ({ page }) => {
  await openConsole(page);
  const overflow = await page.evaluate(
    () => document.documentElement.scrollWidth - document.documentElement.clientWidth,
  );
  expect(overflow, 'the layout overflows horizontally').toBeLessThanOrEqual(1);
});
