/**
 * Holding a moment: click the charts and the console stays in the past until
 * something brings it back.
 *
 * Hover and drag only preview -- the hero's history mode used to vanish the
 * moment the pointer left the plot, so it could not actually be looked at. The
 * unit tests cover the words and the re-find after a refresh (past.test.ts);
 * what only a browser can check is the wiring: the click that pins, and each
 * of the ways back. Both projects run this file -- page.mouse drives a click on
 * the phone project too, and touch.spec.ts covers the tap itself.
 */
import type { Page } from '@playwright/test';
import { expect, openConsole, test } from './harness';

async function holdMoment(page: Page): Promise<void> {
  const plot = page.locator('#cv_t');
  await plot.scrollIntoViewIfNeeded();
  const box = await plot.boundingBox();
  if (!box) throw new Error('the temperature plot has no box to click');
  await page.mouse.click(box.x + box.width * 0.5, box.y + box.height * 0.5);
  // Away from the plots entirely: this is what used to return the hero to now.
  await page.mouse.move(2, 2);
}

/**
 * Pixels of the held crosshair's sand on a plot (charts.ts::holdCrosshair).
 * The canvas is transparent where nothing is drawn, so most of the line reads
 * back as the ink itself; the preview's grey is nowhere near it.
 */
async function sandPixels(page: Page, canvasId: string): Promise<number> {
  return page.evaluate((id) => {
    const c = document.getElementById(id) as HTMLCanvasElement | null;
    const ctx = c?.getContext('2d');
    if (!c || !ctx || c.width === 0 || c.height === 0) return -1;
    const d = ctx.getImageData(0, 0, c.width, c.height).data;
    let n = 0;
    for (let k = 0; k < d.length; k += 4) {
      const near = Math.abs(d[k]! - 217) < 16 && Math.abs(d[k + 1]! - 179) < 16 && Math.abs(d[k + 2]! - 108) < 16;
      if (near && d[k + 3]! > 120) n++;
    }
    return n;
  }, canvasId);
}

test('clicking the chart holds the moment after the pointer leaves', async ({ page }) => {
  await openConsole(page);
  await holdMoment(page);
  await expect(page.locator('#stamp')).toHaveText(/\d{1,2}:\d{2}/);
  await expect(page.locator('#console')).toHaveClass(/\bheld\b/);
  await expect(page.locator('#pastwhen')).toHaveText(/\d{1,2}:\d{2} · .*AGO/);
});

test('Back to now releases a held moment', async ({ page }) => {
  await openConsole(page);
  await holdMoment(page);
  const back = page.locator('#bnow');
  await expect(back).toBeVisible();
  await back.click();
  await expect(page.locator('#stamp')).toHaveText('NOW');
  await expect(page.locator('#console')).not.toHaveClass(/\bpast\b/);
  await expect(back).toBeHidden();
});

test('Escape releases a held moment', async ({ page }) => {
  await openConsole(page);
  await holdMoment(page);
  await expect(page.locator('#stamp')).not.toHaveText('NOW');
  await page.keyboard.press('Escape');
  await expect(page.locator('#stamp')).toHaveText('NOW');
});

test('the way back is a real button, reachable from the keyboard', async ({ page }) => {
  await openConsole(page);
  await holdMoment(page);
  const back = page.locator('#bnow');
  await expect(back).toBeVisible();
  await back.focus();
  await page.keyboard.press('Enter');
  await expect(page.locator('#stamp')).toHaveText('NOW');
});

/**
 * A command is about now: changing the speed while the hero describes 03:00
 * would land the answer under a view that is not showing it.
 */
test('setting a speed while a moment is held returns to now', async ({ page }) => {
  await openConsole(page);
  await holdMoment(page);
  await expect(page.locator('#console')).toHaveClass(/\bheld\b/);
  await page.locator('#stack button').nth(4).click();
  await expect(page.locator('#stamp')).toHaveText('NOW');
  await expect(page.locator('#railnum')).toHaveText('5');
});

/**
 * The click lands on the row the hover is already previewing, so nothing about
 * WHICH moment is shown changes -- only that it is held. The chart has to say
 * so too: a crosshair left in the preview's grey looks like a hover that will
 * vanish, not a moment that stays.
 */
test('a held moment draws its crosshair in the history tone', async ({ page }) => {
  await openConsole(page);
  await holdMoment(page);
  await expect(page.locator('#console')).toHaveClass(/\bheld\b/);
  await expect.poll(() => sandPixels(page, 'cv_t')).toBeGreaterThan(60);
});
