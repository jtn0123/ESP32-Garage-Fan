/**
 * Gestures, which only exist on the phone.
 *
 * A file of its own rather than a runtime skip inside charts.spec.ts: there is
 * no touchscreen in the desk project, so a `test.skip(!hasTouch)` there
 * reported two permanently-skipped tests on every run. A skip that can never
 * turn into a pass carries no information and teaches people to scroll past
 * the skip count -- which is where a REAL skip would then hide. The desktop
 * project excludes this file outright (see playwright.config.ts::testIgnore),
 * so the phone runs these and the desk reports nothing.
 *
 * What is under test is the choice the console has to make on every swipe:
 * scrubbing the chart and scrolling the page are the same gesture until the
 * axis says otherwise, and the page cannot have both.
 */
import { expect, openConsole, test, touchDrag, touchRelease } from './harness';

/**
 * The charts used to be a trap. touchmove called preventDefault() on every
 * move, so a vertical swipe that started anywhere over ~370 px of plots could
 * not scroll the page at all -- on a phone you had to reach around the charts
 * to get past them.
 */
test('a vertical swipe over the charts scrolls the page', async ({ page }) => {
  await openConsole(page);
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
