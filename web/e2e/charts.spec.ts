/**
 * The history area: range switching, the optional rows, and scrubbing.
 *
 * The range buttons are worth real cover because they were wrong on the device
 * twice: /api/history?days= used to default to 1 for a typo'd query (serving
 * RAM-ring data as if it were the card's), and the 7/30-day branch used to
 * return three series instead of seven.
 */
import {
  expect,
  hasTouch,
  inkedColumns,
  openConsole,
  recordRequests,
  test,
  touchDrag,
  touchRelease,
} from './harness';

test('the four ranges are offered and 24H starts selected', async ({ page }) => {
  await openConsole(page);
  await expect(page.locator('#ranges button')).toHaveCount(4);
  await expect(page.locator('#ranges button[data-d="1"]')).toHaveClass(/on/);
  await expect(page.locator('#chtitle')).toHaveText(/24 HOURS/i);
});

for (const [days, title] of [
  ['7', /7 DAYS/i],
  ['30', /30 DAYS/i],
  ['60', /60 DAYS/i],
  ['1', /24 HOURS/i],
] as const) {
  test(`selecting ${days}d refetches that range and relabels the chart`, async ({ page }) => {
    await openConsole(page);
    const fetches = recordRequests(page, /\/api\/history/);
    await page.locator(`#ranges button[data-d="${days}"]`).click();
    await expect(page.locator(`#ranges button[data-d="${days}"]`)).toHaveClass(/on/);
    await expect(page.locator('#chtitle')).toHaveText(title);
    // The request must name the range asked for. A range that quietly serves
    // another window is the exact defect this endpoint already shipped once.
    await expect
      .poll(() => fetches.some((r) => new RegExp(`days=${days}\\b`).test(r.url())))
      .toBeTruthy();
  });
}

/**
 * A relabelled chart is not a plotted one.
 *
 * Every assertion above this was satisfied by a 7D button that fetched, said
 * "LAST 7 DAYS", and drew nothing -- which is exactly what the console did:
 * the wide ranges arrive decimated (~2400 s per row at 7 days against the
 * device's 300 s cadence), and measuring that step against interval_s flagged
 * every row as an outage, so every line broke into invisible one-point
 * segments. Ink on the canvas and a filled readout are what distinguish a
 * range that works from one that only announces itself.
 */
for (const days of ['7', '30', '60'] as const) {
  test(`the ${days}-day range actually plots its data`, async ({ page }) => {
    await openConsole(page);
    await page.locator(`#ranges button[data-d="${days}"]`).click();
    await expect(page.locator('#chtitle')).toHaveText(new RegExp(`${days} DAYS`, 'i'));
    await expect
      .poll(() => inkedColumns(page, 'cv_t'), {
        message: `the ${days}-day chart is blank`,
        timeout: 15_000,
      })
      .toBeGreaterThan(100);
    await expect(page.locator('#roT')).toContainText('°');
    // And nothing apologises: a working range needs no explanation.
    await expect(page.locator('#tcap')).not.toContainText(/could not|no samples/i);
  });
}

test('only one range is selected at a time', async ({ page }) => {
  await openConsole(page);
  await page.locator('#ranges button[data-d="7"]').click();
  await expect(page.locator('#ranges button[data-d="7"]')).toHaveClass(/on/);
  await expect(page.locator('#ranges button.on')).toHaveCount(1);
});

test('the temperature chart always draws', async ({ page }) => {
  await openConsole(page);
  const painted = await page.evaluate(() => {
    const c = document.getElementById('cv_t') as HTMLCanvasElement | null;
    return !!c && c.width > 0 && c.height > 0;
  });
  expect(painted, '#cv_t has no drawing surface').toBeTruthy();
});

// ------------------------------------------------------------- the extra rows

// Each chip owns exactly one row. Asserting the MAPPING, not just "something
// appeared", is what catches a chip wired to the wrong series.
const CHIP_ROWS = [
  { key: 'fan', row: '#sub_s' },
  { key: 'humidity', row: '#sub_h' },
  { key: 'pressure', row: '#sub_p' },
  { key: 'battery', row: '#sub_b' },
  { key: 'power', row: '#sub_w' },
  { key: 'voc', row: '#sub_v' },
  { key: 'nox', row: '#sub_n' },
] as const;

test('every series has a chip', async ({ page }) => {
  await openConsole(page);
  await expect(page.locator('#chips button')).toHaveCount(CHIP_ROWS.length);
  for (const { key } of CHIP_ROWS) {
    await expect(page.locator(`#chips button[data-key="${key}"]`)).toBeVisible();
  }
});

for (const { key, row } of CHIP_ROWS) {
  test(`the ${key} chip toggles ${row} and nothing else`, async ({ page }) => {
    await openConsole(page);
    const chip = page.locator(`#chips button[data-key="${key}"]`);
    const target = page.locator(row);
    const others = CHIP_ROWS.filter((c) => c.row !== row).map((c) => c.row);
    const otherState = async () =>
      Promise.all(others.map((sel) => page.locator(sel).getAttribute('class')));

    // FAN starts shown and the rest start hidden, so read the state rather than
    // assuming it -- the test has to work whichever way the default goes.
    const shownBefore = !((await target.getAttribute('class')) ?? '').includes('hide');
    const before = await otherState();

    await chip.click();
    await expect(target).toHaveClass(shownBefore ? /hide/ : /^(?!.*hide).*$/);
    expect(await otherState(), `toggling ${key} disturbed another row`).toEqual(before);

    await chip.click();
    await expect(target).toHaveClass(shownBefore ? /^(?!.*hide).*$/ : /hide/);
  });
}

// ---------------------------------------------------------------- scrubbing

/**
 * Scrubbing repaints the HERO, not a tooltip: the big reading becomes the value
 * at the moment under the cursor and the stamp stops saying NOW. That is the
 * whole feature -- "drag across to read any moment", as the caption promises.
 */
test('dragging across the plots reads out a past moment', async ({ page }) => {
  await openConsole(page);
  const plots = page.locator('#plots');
  await expect(plots).toBeVisible();
  await expect(page.locator('#stamp')).toHaveText('NOW');
  const box = await plots.boundingBox();
  expect(box).not.toBeNull();
  if (!box) return;
  await page.mouse.move(box.x + box.width * 0.5, box.y + box.height * 0.4);
  await expect(page.locator('#stamp')).not.toHaveText('NOW');
});

test('leaving the plots returns the hero to now', async ({ page }) => {
  await openConsole(page);
  const box = await page.locator('#plots').boundingBox();
  if (!box) return;
  await page.mouse.move(box.x + box.width * 0.5, box.y + box.height * 0.4);
  await expect(page.locator('#stamp')).not.toHaveText('NOW');
  // Out of the plots entirely, which is what fires mouseleave.
  await page.mouse.move(box.x - 60, box.y - 60);
  await expect(page.locator('#stamp')).toHaveText('NOW');
});

/**
 * The moment being pointed at has to be READABLE, and not only under the
 * finger. On a phone the hero stamp has scrolled off the top by the time a
 * thumb reaches the plots and the hand covers the crosshair, so the chart
 * header carries it too -- that is the only copy a touch user can see.
 */
test('scrubbing names the moment above the plots', async ({ page }) => {
  await openConsole(page);
  await expect(page.locator('#chtitle')).toHaveText(/24 HOURS/i);
  const box = await page.locator('#cv_t').boundingBox();
  if (!box) return;
  await page.mouse.move(box.x + box.width * 0.5, box.y + box.height * 0.5);
  // A clock, not merely "not NOW": a scrub readout with no time answers nothing.
  await expect(page.locator('#chtitle')).toHaveText(/\d{1,2}:\d{2}/);
  await expect(page.locator('#stamp')).toHaveText(/\d{1,2}:\d{2}/);
  // And the per-row readout tracks the same sample.
  await expect(page.locator('#roT')).toContainText('°');
});

test('a scrubbed moment on a multi-day range carries its date', async ({ page }) => {
  await openConsole(page);
  await page.locator('#ranges button[data-d="7"]').click();
  await expect(page.locator('#chtitle')).toHaveText(/7 DAYS/i);
  const box = await page.locator('#cv_t').boundingBox();
  if (!box) return;
  await page.mouse.move(box.x + box.width * 0.4, box.y + box.height * 0.5);
  // "15:27" happens once a day for a week; the day is the point of the range.
  await expect(page.locator('#chtitle')).toHaveText(/\d{1,2}\/\d{1,2} \d{1,2}:\d{2}/);
});

// ------------------------------------------------------------ touch gestures

/**
 * The charts used to be a trap. touchmove called preventDefault() on every
 * move, so a vertical swipe that started anywhere over ~400 px of plots could
 * not scroll the page at all -- on a phone you had to reach around the charts
 * to get past them.
 */
test('a vertical swipe over the charts scrolls the page', async ({ page }) => {
  await openConsole(page);
  test.skip(!(await hasTouch(page)), 'no touchscreen on the desk project');
  await page.locator('#cv_t').scrollIntoViewIfNeeded();
  const box = await page.locator('#cv_t').boundingBox();
  if (!box) return;
  const before = await page.evaluate(() => window.scrollY);
  const x = box.x + box.width * 0.5;
  await touchDrag(page, { x, y: box.y + box.height * 0.8 }, { x, y: box.y + box.height * 0.8 - 200 });
  await expect
    .poll(() => page.evaluate(() => window.scrollY), {
      message: 'a vertical swipe over the plots did not scroll the page',
    })
    .toBeGreaterThan(before);
  // And it must not have scrubbed on the way past.
  await expect(page.locator('#stamp')).toHaveText('NOW');
});

test('a horizontal drag scrubs without moving the page', async ({ page }) => {
  await openConsole(page);
  test.skip(!(await hasTouch(page)), 'no touchscreen on the desk project');
  await page.locator('#cv_t').scrollIntoViewIfNeeded();
  const box = await page.locator('#cv_t').boundingBox();
  if (!box) return;
  const before = await page.evaluate(() => window.scrollY);
  const y = box.y + box.height * 0.5;
  await touchDrag(page, { x: box.x + box.width * 0.25, y }, { x: box.x + box.width * 0.75, y }, {
    hold: true, // releasing ends the scrub, so read it with the finger still down
  });
  await expect(page.locator('#stamp')).not.toHaveText('NOW');
  expect(await page.evaluate(() => window.scrollY), 'the page moved under the scrub').toBe(before);
  await touchRelease(page);
  await expect(page.locator('#stamp')).toHaveText('NOW');
});

// The explanations behind the status strip. Note the handlers sit on .bit, not
// on the value span inside it -- a selector that lands on the child silently
// clicks nothing, which is how this test failed the first time.
test('a status bit explains itself on hover', async ({ page }) => {
  await openConsole(page);
  const bits = page.locator('#stats .bit');
  expect(await bits.count(), 'the status strip rendered nothing').toBeGreaterThan(0);
  await expect(page.locator('#tip')).toHaveClass(/hide/);
  await bits.first().hover();
  await expect(page.locator('#tip')).not.toHaveClass(/hide/);
  await expect(page.locator('#tipt')).not.toBeEmpty();
  await expect(page.locator('#tipb')).not.toBeEmpty();
});

/**
 * Hover is useless on the phone in the garage, so the bit is click-to-toggle
 * as well. Written as a TOGGLE rather than "click shows it" on purpose: with a
 * real pointer the click is preceded by mouseenter, which has already opened
 * the tip, so the click closes it. Asserting "click shows" passes only on
 * touch and fails on a desk -- which is exactly how this test failed first.
 */
test('a status bit toggles on click', async ({ page }) => {
  await openConsole(page);
  const bit = page.locator('#stats .bit').first();
  const hidden = async () =>
    ((await page.locator('#tip').getAttribute('class')) ?? '').includes('hide');

  await bit.click();
  const afterFirst = await hidden();
  await bit.click();
  expect(await hidden(), 'a second click did not toggle the tip back').toBe(!afterFirst);
  await bit.click();
  expect(await hidden()).toBe(afterFirst);
});

test('each status bit explains something different', async ({ page }) => {
  await openConsole(page);
  const bits = page.locator('#stats .bit');
  const seen = new Set<string>();
  const n = Math.min(await bits.count(), 4);
  for (let i = 0; i < n; i++) {
    await bits.nth(i).hover();
    await expect(page.locator('#tip')).not.toHaveClass(/hide/);
    seen.add(((await page.locator('#tipt').textContent()) ?? '').trim());
  }
  expect(seen.size, 'status bits share a tooltip title').toBe(n);
});

/**
 * The gas rows during warm-up. The mock's default history has the SGP41
 * appear a third of the way in and its indices go live at two thirds, so the
 * VOC row must draw SOMETHING (the raw ticks) rather than a flat zero, and
 * the readout must say which regime the newest sample is in.
 */
test('the VOC row shows data and a labelled readout', async ({ page }) => {
  await openConsole(page);
  await page.locator('#chips button[data-key="voc"]').click();
  await expect(page.locator('#sub_v')).not.toHaveClass(/hide/);
  // The newest mock samples carry live indices, so the readout names one.
  await expect(page.locator('#roV')).toContainText(/index \d+/, { timeout: 15_000 });
  await expect(page.locator('#roV')).toContainText(/raw \d+/);
});

test('the power row plots the plug watts', async ({ page }) => {
  await openConsole(page);
  await page.locator('#chips button[data-key="power"]').click();
  await expect(page.locator('#sub_w')).not.toHaveClass(/hide/);
  await expect(page.locator('#roW')).toContainText(/W$/, { timeout: 15_000 });
});

test('an outage is explained, not just left as a hole', async ({ page }) => {
  // The chart already breaks its lines across a gap. What this pins is the
  // half that was missing: the console asks WHY, and keeps drawing when the
  // answer is unavailable.
  const boots = recordRequests(page, /\/api\/boots/);
  await openConsole(page);
  await expect.poll(() => boots.length).toBeGreaterThan(0);
  await expect(boots[0]!.url()).toMatch(/days=\d+/);
});

test('a controller with no restart marks still draws its charts', async ({ page }) => {
  // /api/boots is a separate request precisely so it can fail alone: an older
  // firmware has no such route, and a missing explanation must never withhold
  // the data it would have explained.
  await page.route('**/api/boots*', (r) => r.fulfill({ status: 404, body: '{"error":"nope"}' }));
  await openConsole(page);
  await expect(page.locator('#cv_t')).toBeVisible();
  await expect(page.locator('#tcap')).not.toContainText(/could not load/i);
});
