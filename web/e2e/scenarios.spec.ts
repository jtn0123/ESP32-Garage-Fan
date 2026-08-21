/**
 * The states the fan reaches rarely and the console has to survive anyway.
 *
 * SERIAL, and every test restores the knobs afterwards: mock_device.py holds
 * SCEN in module state, so a leaked knob would break the next test on this
 * worker rather than failing here. (It can no longer reach any OTHER worker --
 * harness.ts gives each one its own mock process.)
 *
 * These are the cases dogfooding found the hard way -- an unmounted card, an
 * outage mid-history, a nearly-empty card, a controller that stops answering.
 * A console that throws on any of them is a console you cannot use at the exact
 * moment you need it.
 */
import { expect, inkedColumns, openConsole, resetScen, scen, test } from './harness';

// Serial within the file: these tests hand the knobs back and forth, and one
// starting before the previous one has reset them would read the wrong state.
// Isolation FROM OTHER FILES is no longer this file's problem -- harness.ts
// gives every worker its own mock process, so `down=true` can only ever affect
// the worker that set it.
test.describe.configure({ mode: 'serial' });

test.afterEach(async ({ page }) => {
  await resetScen(page);
  // Scenario knobs are not the only shared mock state: the DRAW spec pins the
  // speed, and 9 is the mock's boot value, so every test starts from it.
  await page.request.post('/api/set?speed=9');
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

/**
 * NOT fully covered, and worth saying so rather than implying otherwise.
 *
 * The title is the intent; what is asserted is weaker. Column-ink cannot tell a
 * bridged line from a real break, because the shaded band between garage and
 * yard inks the gap's columns either way -- measured at 1100 inked columns with
 * a gap against 1097 without. Proving the line itself breaks needs the series
 * state exposed to the page, or a visual-regression pass. What IS asserted:
 * the page survives the gap and scrubbing across it lands on a real sample.
 */
test('an outage mid-history does not break the page', async ({ page }) => {
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

/**
 * A window the card cannot fill has to SAY so.
 *
 * Picking 7D on a device that has only been logging a day drew a day's data
 * squeezed into the right-hand seventh of the chart with nothing to explain
 * the empty five sixths -- and on a phone, where the caption is hidden, there
 * was nowhere for an explanation to appear at all. The `note` class is what
 * makes an explanation (as opposed to the standing hint) visible at that
 * width, so this asserts visibility, not just text.
 */
test('a range wider than the card explains the empty stretch', async ({ page }) => {
  await scen(page, { rows: '288' }); // exactly one day on the card
  await openConsole(page);
  await page.locator('#ranges button[data-d="7"]').click();
  await expect(page.locator('#tcap')).toContainText(/only goes back/i);
  await expect(page.locator('#tcap')).toBeVisible();
});

test('a range with nothing in it explains the blank', async ({ page }) => {
  await scen(page, { rows: '0' });
  await openConsole(page);
  await expect(page.locator('#tcap')).toContainText(/no samples in this range/i);
  await expect(page.locator('#tcap')).toBeVisible();
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
  // And the plot is genuinely BLANK, not the previous range's data sitting
  // under a new label. The caption alone cannot tell those apart, and stale
  // data wearing a fresh label is the failure worth catching here.
  await expect
    .poll(() => inkedColumns(page, 'cv_t'), {
      message: 'the chart still has data drawn on it after a failed load',
      timeout: 15_000,
    })
    .toBe(0);
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
  // Recovery has to REDRAW, not merely stop apologising: a cleared caption over
  // an empty plot is still a broken page.
  await expect
    .poll(() => inkedColumns(page, 'cv_t'), {
      message: 'the error cleared but nothing was plotted',
      timeout: 15_000,
    })
    .toBeGreaterThan(0);
});

/**
 * The watt meter on the fan's supply -- the link's only feedback.
 *
 * The DRAW cell prefers the measured watts over the cubic estimate, the PLUG
 * status bit carries watts and volts, and a sustained disagreement raises the
 * one alert this page can raise that nothing else can: the fan ran at full
 * power for a day while everything on the device honestly said OFF
 * (2026-08-13), and only the meter could have said so.
 */
test('the DRAW cell shows the measured watts, not the estimate', async ({ page }) => {
  await openConsole(page);
  // Pin the speed: specs from other files share this worker's mock and leave
  // whatever speed they last set. At 9 the baseline table says 20.3 W -- the
  // cubic estimate said 47 W, which is what this cell used to show.
  await page.request.post('/api/set?speed=9');
  await expect(page.locator('#mW')).toHaveText('20.3 W', { timeout: 15_000 });
  await expect(page.locator('#stats')).toContainText('20.3 W · 120.9 V');
});

test('a plug disagreement raises the warning banner', async ({ page }) => {
  await scen(page, { plug: 'bad' });
  await openConsole(page);
  await expect(page.locator('#plugwarn')).toBeVisible();
  await expect(page.locator('#plugwarn')).toContainText(/does not match/i);
});

test('a cycling fan raises the cycling banner, not the plain disagreement', async ({ page }) => {
  // The 2026-08-20 night: one speed held for nine hours while the meter saw
  // the fan stop and restart every minute or two. The banner has to name
  // THAT -- the plain "does not match" is what it showed, and it read as a
  // noisy meter.
  await scen(page, { plug: 'cycling' });
  await openConsole(page);
  await expect(page.locator('#plugwarn')).toBeVisible();
  await expect(page.locator('#plugwarn')).toContainText(/cycling on and off/i);
  await expect(page.locator('#plugwarn')).toContainText(/5 times in the last 10 minutes/);
  await expect(page.locator('#plugwarn')).not.toContainText(/does not match/i);
  await expect(page.locator('#stats')).toContainText('CYCLING ×5');
});

test('the power row tints the buckets the meter saw the fan cycling in', async ({ page }) => {
  await scen(page, { plug: 'cycling' });
  await openConsole(page);
  await page.locator('#chips button[data-key="power"]').click();
  await expect(page.locator('#sub_w')).not.toHaveClass(/hide/);
  // The newest rows under this scenario carry a flip count, so the readout
  // (pinned to the latest sample until a scrub) names it.
  await expect(page.locator('#roW')).toContainText(/cycling ×\d/, { timeout: 15_000 });
  // And the canvas is drawn: the mock's last two hours are flagged, so the
  // row must have inked columns (the tint's exact pixels are not asserted --
  // see inkedColumns on what a canvas probe can and cannot resolve).
  await expect.poll(() => inkedColumns(page, 'cv_w'), { timeout: 15_000 }).toBeGreaterThan(0);
});

test('no meter means the estimate and no banner, not a crash', async ({ page }) => {
  await scen(page, { plug: 'none' });
  await openConsole(page);
  await expect(page.locator('#plugwarn')).toBeHidden();
  await expect(page.locator('#stats')).toContainText('no meter');
  // The estimate path still fills DRAW from /api/stats.
  await expect(page.locator('#mW')).toHaveText(/W$/);
});

/**
 * The device stops answering, and the page has to stop looking live.
 *
 * The CSS for this existed for a round without ever being reachable: the
 * verdict was gated on `view.lastOk`, the threshold was 45 s (three missed
 * polls), and nothing repainted between the failure and the verdict. A garage
 * console that shows a confident 74.9 under a `NOW` badge while the controller
 * is dark is the one failure mode that gets acted on.
 */
test('a controller that stops answering stops looking live', async ({ page }) => {
  // Real wall-clock behaviour, so it needs real wall clock: the badge decays on
  // the first missed poll (~15 s) and the verdict lands on the second (~30 s),
  // which is past the suite's default 30 s budget on its own.
  test.setTimeout(90_000);
  await openConsole(page);
  await expect(page.locator('#hmq')).toContainText('BROKER');
  await scen(page, { down: 'true' });

  await expect(page.locator('#hmq')).toHaveText('● NO CONTACT', { timeout: 40_000 });
  await expect(page.locator('#hmq')).toHaveClass(/stale/);
  await expect(page.locator('body')).toHaveClass(/offline/);
  // The badge stops asserting NOW on the FIRST failed poll, before the verdict,
  // so the reading's age is visible while it decays.
  await expect(page.locator('#stamp')).not.toHaveText('NOW');
  await expect(page.locator('#stamp')).toHaveText(/AGO|NO CONTACT/);
});

test('a console opened cold against a dead controller says so', async ({ page }) => {
  test.setTimeout(90_000);
  // The realistic case: the fan is down, so you open the page to find out why.
  // Nothing has ever succeeded here, which is exactly what used to leave it on
  // "Connecting to the controller…" forever.
  await scen(page, { down: 'true' });
  await page.goto('/');
  await expect(page.locator('#reason')).toContainText(/No answer from the controller/i, {
    timeout: 40_000,
  });
  await expect(page.locator('#hmq')).toHaveText('● NO CONTACT');
  await expect(page.locator('body')).toHaveClass(/offline/);
  // And it must not claim a reading it never had.
  await expect(page.locator('#stamp')).not.toHaveText('NOW');
});
