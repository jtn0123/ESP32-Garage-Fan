/**
 * The e-ink mirror in the settings drawer.
 *
 * The point of the mirror is that it CANNOT drift from the wall: the device
 * sends the same two 1-bit planes it clocked to the glass and the console only
 * blits them. So the thing worth testing is the blit -- that the bytes on the
 * wire become the right pixels on the canvas -- not that some layout was
 * reproduced.
 *
 * Verified by reading pixels back out of the canvas, because "the canvas has a
 * size" passes just as happily when it is blank, and a blank e-ink mirror looks
 * exactly like a panel that has not refreshed yet.
 */
import { expect, openConsole, test } from './harness';

async function openPanel(page: import('@playwright/test').Page): Promise<void> {
  await openConsole(page);
  await page.locator('#nav').click();
  await expect(page.locator('#settings')).not.toHaveClass(/hide/);
  await expect(page.locator('#pnlcv')).toBeAttached();
}

test('the mirror paints the frame the device sent', async ({ page }) => {
  await openPanel(page);
  // The canvas is resized to the frame's dimensions when it paints, so a
  // still-default 300x150 means nothing was drawn.
  await expect
    .poll(() => page.locator('#pnlcv').evaluate((c) => (c as HTMLCanvasElement).width), {
      message: 'the panel canvas never took the frame size',
      timeout: 15_000,
    })
    .toBe(500); // 250 px panel at scale 2
  await expect(page.locator('#pnlcv')).toHaveJSProperty('height', 244);
});

test('the mirror draws real ink, not an empty frame', async ({ page }) => {
  await openPanel(page);
  await expect.poll(() => inkFraction(page), { timeout: 15_000 }).toBeGreaterThan(0.01);
});

/**
 * THE ONE THAT MATTERS: the bytes decode to the right pixels.
 *
 * The plane format is row-major, MSB first. Getting the bit order backwards
 * mirrors every glyph horizontally and still produces a picture that looks
 * plausible at a glance -- I misread exactly that off a scaled screenshot while
 * building this and called it a bug before checking the pixels. Reading a known
 * glyph out of the canvas is what settles it.
 *
 * The mock draws its temperature with a 3x5 font at scale 4, so "7" is a full
 * top bar with the stem on the RIGHT. Mirrored, the stem lands on the left.
 */
test('the plane bit order is not mirrored', async ({ page }) => {
  await openPanel(page);
  await expect.poll(() => inkFraction(page), { timeout: 15_000 }).toBeGreaterThan(0.01);

  const rows = await page.locator('#pnlcv').evaluate((el) => {
    const c = el as HTMLCanvasElement;
    const ctx = c.getContext('2d');
    if (!ctx) return [];
    const scale = c.width / 250;
    const d = ctx.getImageData(0, 0, c.width, c.height).data;
    const ink = (px: number, py: number) => {
      const i = ((Math.floor(py * scale) * c.width) + Math.floor(px * scale)) * 4;
      return (d[i] ?? 255) < 160;
    };
    const out: string[] = [];
    for (let y = 26; y < 46; y += 2) {
      let s = '';
      for (let x = 6; x < 20; x++) s += ink(x, y) ? '#' : '.';
      out.push(s);
    }
    return out;
  });

  expect(rows.length).toBeGreaterThan(4);
  const top = rows[0] ?? '';
  const below = rows[3] ?? '';
  // The bar across the top of the 7.
  expect(top.replace(/\./g, '').length, 'no top bar where the glyph should be').toBeGreaterThan(6);
  // ...and the stem under it sits on the RIGHT of that bar, not the left.
  const stem = below.indexOf('#');
  const barStart = top.indexOf('#');
  expect(stem, 'the glyph has no stem below its bar').toBeGreaterThanOrEqual(0);
  expect(stem, 'the glyph is mirrored: the stem is on the left').toBeGreaterThan(barStart + 2);
});

test('the mirror says how stale it is and which panel is fitted', async ({ page }) => {
  await openPanel(page);
  // The panel refreshes on a 5-minute cadence, so a few minutes old is normal.
  // Saying so is what stops a correct mirror looking like a broken one.
  await expect(page.locator('#pnlmsg')).toContainText(/refreshed|waiting/i);
  // The mock stands in for the mono part, so it must say the accents are grey
  // rather than silently showing a colour the glass cannot produce.
  await expect(page.locator('#pnlmsg')).toContainText(/mono panel/i);
});

test('the refresh button asks the device to repaint', async ({ page }) => {
  await openPanel(page);
  const posts: string[] = [];
  page.on('request', (r) => {
    if (/\/api\/display\/refresh/.test(r.url())) posts.push(r.method());
  });
  await page.locator('#pnlrefresh').click();
  await expect.poll(() => posts.length, { timeout: 15_000 }).toBeGreaterThan(0);
  // A GET that repaints is reachable from a prefetch; this one writes.
  expect(posts).not.toContain('GET');
});

/** Fraction of sampled canvas pixels that are darker than the paper tone. */
async function inkFraction(page: import('@playwright/test').Page): Promise<number> {
  return page.locator('#pnlcv').evaluate((el) => {
    const c = el as HTMLCanvasElement;
    if (!c.width || !c.height) return 0;
    const ctx = c.getContext('2d');
    if (!ctx) return 0;
    const d = ctx.getImageData(0, 0, c.width, c.height).data;
    let ink = 0;
    let n = 0;
    for (let i = 0; i < d.length; i += 4 * 16) {
      n++;
      if ((d[i] ?? 255) < 200) ink++;
    }
    return n ? ink / n : 0;
  });
}
