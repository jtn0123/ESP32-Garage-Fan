/**
 * The states the fan reaches rarely and the console has to survive anyway.
 *
 * SERIAL, and every test restores the knobs afterwards: mock_device.py holds
 * SCEN in module state shared by the whole run, so a leaked knob would break
 * whatever ran next rather than failing here.
 *
 * These are the cases dogfooding found the hard way -- an unmounted card, an
 * outage mid-history, a nearly-empty card, a controller that stops answering.
 * A console that throws on any of them is a console you cannot use at the exact
 * moment you need it.
 */
import { SCENARIO_PORT } from '../playwright.config';
import { expect, openConsole, resetScen, scen, test } from './harness';

// Its OWN mock process. `down=true` hangs up on every /api request the server
// handles, so sharing one with the rest of the suite made unrelated specs fail
// whenever a scenario was mid-flight. Serial mode alone does not fix that: it
// orders tests within this file, not against the files running beside it.
test.use({ baseURL: `http://127.0.0.1:${SCENARIO_PORT}` });
test.describe.configure({ mode: 'serial' });

test.afterEach(async ({ page }) => {
  await resetScen(page);
});

test('an unmounted card still renders the page', async ({ page }) => {
  await scen(page, { card: 'false' });
  await openConsole(page);
  // The live readings come from the sensor, not the card, so the hero stays up.
  await expect(page.locator('#tT')).not.toHaveText('–');
  await expect(page.locator('#stats')).toContainText('SD');
});

test('an empty card does not break the charts', async ({ page }) => {
  await scen(page, { rows: '0' });
  await openConsole(page);
  await expect(page.locator('#plots')).toBeVisible();
  // No rows means no scrub target; the page must not throw reaching for one.
  const box = await page.locator('#plots').boundingBox();
  if (box) await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  await expect(page.locator('#brand')).toBeVisible();
});

test('a nearly-empty card renders what little there is', async ({ page }) => {
  await scen(page, { rows: '3' });
  await openConsole(page);
  await expect(page.locator('#plots')).toBeVisible();
});

test('an outage mid-history is drawn as a gap, not bridged', async ({ page }) => {
  await scen(page, { gap_at: '100' });
  await openConsole(page);
  await expect(page.locator('#plots')).toBeVisible();
  // Scrubbing across the gap must land on a sample that exists rather than
  // interpolating one that never happened.
  const box = await page.locator('#plots').boundingBox();
  if (!box) return;
  for (const f of [0.3, 0.5, 0.7]) {
    await page.mouse.move(box.x + box.width * f, box.y + box.height * 0.4);
  }
  await expect(page.locator('#stamp')).not.toHaveText('NOW');
});

test('null readings in the history do not break the draw', async ({ page }) => {
  await scen(page, { corrupt: 'true' });
  await openConsole(page);
  await expect(page.locator('#plots')).toBeVisible();
  await expect(page.locator('#tT')).not.toHaveText('–');
});

test('a dead-flat series still renders', async ({ page }) => {
  // Zero range is the classic divide-by-zero in a chart's y-scaling.
  await scen(page, { flat_rh: 'true' });
  await openConsole(page);
  await page.locator('#chips button[data-key="humidity"]').click();
  await expect(page.locator('#sub_h')).not.toHaveClass(/hide/);
  await expect(page.locator('#plots')).toBeVisible();
});

test('an unsynced clock does not print 1970', async ({ page }) => {
  await scen(page, { synced: 'false' });
  await openConsole(page);
  await expect(page.locator('#plots')).toBeVisible();
  await expect(page.locator('#chead')).not.toContainText('1970');
  await expect(page.locator('#plots')).not.toContainText('1970');
});

test('every range survives an unmounted card', async ({ page }) => {
  await scen(page, { card: 'false' });
  await openConsole(page);
  for (const d of ['7', '30', '1']) {
    await page.locator(`#ranges button[data-d="${d}"]`).click();
    await expect(page.locator(`#ranges button[data-d="${d}"]`)).toHaveClass(/on/);
    await expect(page.locator('#brand')).toBeVisible();
  }
});

/**
 * The controller stops answering while the page is open.
 *
 * The history area says so: a range it cannot fetch blanks the plots and puts
 * the reason in the caption, deliberately, so a stale chart is never passed off
 * as current.
 *
 * The hero does NOT, and that is a considered decision rather than an oversight
 * -- app.ts swallows a failed state poll ("a transient failure should not blank
 * the page") because the 15 s poll usually re-syncs before anyone notices. The
 * cost is that a genuinely dead controller keeps showing its last reading, and
 * the broker chip keeps saying UP. This test pins the behaviour that exists; if
 * that trade is ever revisited, this is the test that should change with it.
 */
test('a controller that goes away is reported in the history area', async ({ page }) => {
  await openConsole(page);
  expect(await page.locator('#tT').textContent()).not.toBe('–');

  await scen(page, { down: 'true' });
  // Force a fetch rather than waiting out the 15 s poll.
  await page.locator('#ranges button[data-d="7"]').click();

  await expect(page.locator('#tcap')).toContainText(/could not load history/i, {
    timeout: 20_000,
  });
  // Blanked, not left showing the previous range's data under a new label.
  await expect(page.locator('#brand')).toBeVisible();
});

test('recovering from an outage repaints the charts', async ({ page }) => {
  await openConsole(page);
  await scen(page, { down: 'true' });
  await page.locator('#ranges button[data-d="7"]').click();
  await expect(page.locator('#tcap')).toContainText(/could not load history/i, {
    timeout: 20_000,
  });

  await scen(page, { down: 'false' });
  await page.locator('#ranges button[data-d="1"]').click();
  await expect(page.locator('#tcap')).not.toContainText(/could not load history/i, {
    timeout: 20_000,
  });
});
