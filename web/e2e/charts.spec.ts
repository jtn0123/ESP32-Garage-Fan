/**
 * The history area: range switching, the optional rows, and scrubbing.
 *
 * The range buttons are worth real cover because they were wrong on the device
 * twice: /api/history?days= used to default to 1 for a typo'd query (serving
 * RAM-ring data as if it were the card's), and the 7/30-day branch used to
 * return three series instead of seven.
 */
import { expect, openConsole, recordRequests, test } from './harness';

test('the three ranges are offered and 24H starts selected', async ({ page }) => {
  await openConsole(page);
  await expect(page.locator('#ranges button')).toHaveCount(3);
  await expect(page.locator('#ranges button[data-d="1"]')).toHaveClass(/on/);
  await expect(page.locator('#chtitle')).toHaveText(/24 HOURS/i);
});

for (const [days, title] of [
  ['7', /7 DAYS/i],
  ['30', /30 DAYS/i],
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
