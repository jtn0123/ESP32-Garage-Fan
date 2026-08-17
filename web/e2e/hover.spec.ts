/**
 * Reading by hovering, which only exists on a desk.
 *
 * The mirror image of touch.spec.ts, and it exists for the same reason: the
 * status bits used to bind mouseenter/mouseleave on every device, and on a phone
 * that pointer-enter compatibility event opened the tip a fraction before the
 * click closed it again -- one tap, no tooltip, six dead affordances. The hover
 * pair is now bound only where `(hover: hover)` matches, so asserting it on an
 * emulated phone would be asserting a fiction the hardware cannot produce.
 * Playwright can synthesise that hover; a finger cannot.
 *
 * The mobile project excludes this file (see playwright.config.ts::testIgnore);
 * touch.spec.ts covers the same six affordances by tapping them.
 */
import { expect, openConsole, test } from './harness';

// Note the handlers sit on .bit, not on the value span inside it -- a selector
// that lands on the child silently clicks nothing, which is how this test failed
// the first time.
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
