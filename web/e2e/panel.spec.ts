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
import { expect, openConsole, scen, test } from './harness';

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
 * THE ONE THAT MATTERS: the bytes become the right pixels.
 *
 * Decoded independently here from the served base64 using the documented
 * convention -- row-major, MSB first, set bit = ink, red over black -- and
 * compared against what the canvas actually shows. Getting the bit order
 * backwards mirrors every glyph horizontally and still renders a picture that
 * looks plausible at a glance; I misread exactly that off a scaled screenshot
 * while building this, called it a bug, and was wrong.
 *
 * Deliberately NOT keyed on the shape of a particular glyph. The first version
 * of this test asserted the stem position of a "7", and it broke the moment the
 * mock drew a different temperature -- a test that fails when the DATA changes
 * is testing the data, not the blit.
 */
test('every pixel matches an independent decode of the served frame', async ({ page }) => {
  await openPanel(page);
  await expect.poll(() => inkFraction(page), { timeout: 15_000 }).toBeGreaterThan(0.01);

  const frame = await (await page.request.get('/api/display')).json();
  const black = Buffer.from(frame.black, 'base64');
  const red = Buffer.from(frame.red, 'base64');
  const { w, h, stride, tricolor } = frame;
  const bit = (p: Buffer, x: number, y: number) =>
    ((p[y * stride + (x >> 3)] ?? 0) & (0x80 >> (x & 7))) !== 0;

  const shown = await page.locator('#pnlcv').evaluate((el) => {
    const c = el as HTMLCanvasElement;
    const ctx = c.getContext('2d');
    if (!ctx) return null;
    const d = ctx.getImageData(0, 0, c.width, c.height).data;
    return { scale: c.width / 250, w: c.width, px: Array.from(d) };
  });
  expect(shown).not.toBeNull();
  if (!shown) return;

  // A stride-7 walk over the whole panel: dense enough to catch a mirror or an
  // off-by-one row, cheap enough to run in a test.
  let compared = 0;
  for (let y = 0; y < h; y += 7) {
    for (let x = 0; x < w; x += 7) {
      const sx = Math.floor(x * shown.scale);
      const sy = Math.floor(y * shown.scale);
      const i = (sy * shown.w + sx) * 4;
      const r = shown.px[i] ?? 0;
      const g = shown.px[i + 1] ?? 0;
      const isInk = r < 90 && g < 90;
      const isRed = r > 150 && g < 90;
      const wantRed = tricolor && bit(red, x, y);
      const wantInk = !wantRed && bit(black, x, y);
      if (wantRed) {
        expect(isRed, `(${x},${y}) should be red`).toBe(true);
      } else {
        expect(isInk, `(${x},${y}) ink mismatch`).toBe(wantInk);
      }
      compared++;
    }
  }
  expect(compared, 'no pixels were compared').toBeGreaterThan(400);
});

test('the mirror says how stale it is and which panel is fitted', async ({ page }) => {
  await openPanel(page);
  // The panel refreshes on a 5-minute cadence, so a few minutes old is normal.
  // Saying so is what stops a correct mirror looking like a broken one.
  await expect(page.locator('#pnlmsg')).toContainText(/refreshed|waiting/i);
  // The mock stands in for the tricolor part the device turned out to carry
  // (the glass showed its header rule in red, 2026-08-12), so the mirror must
  // say so rather than calling the accents grey.
  await expect(page.locator('#pnlmsg')).toContainText(/tricolor panel/i);
  // ...and the red plane must actually be painted. The mock's header rule is
  // red-only, so a mirror with zero red pixels is dropping a plane the glass
  // shows.
  await expect
    .poll(
      () =>
        page.locator('#pnlcv').evaluate((el) => {
          const c = el as HTMLCanvasElement;
          const ctx = c.getContext('2d');
          if (!ctx) return -1;
          const d = ctx.getImageData(0, 0, c.width, c.height).data;
          let n = 0;
          for (let i = 0; i < d.length; i += 4) {
            const r = d[i] ?? 0, g = d[i + 1] ?? 0, b = d[i + 2] ?? 0;
            if (r > 150 && g < 90 && b < 90) n++;
          }
          return n;
        }),
      { message: 'a tricolor panel mirror painted no red pixels', timeout: 15_000 },
    )
    .toBeGreaterThan(0);
});

test('the refresh button asks the device to repaint', async ({ page }) => {
  await openPanel(page);
  const posts: string[] = [];
  page.on('request', (r) => {
    if (/\/api\/display\/refresh/.test(r.url())) posts.push(r.method());
  });
  await page.locator('#pnlrefresh').click();
  await expect.poll(() => posts.length, { timeout: 15_000 }).toBeGreaterThan(0);
  // Named positively: `not.toContain('GET')` also passes for HEAD or PUT, and
  // the claim is that this route writes. net/web.cpp registers it HTTP_POST.
  expect(posts).toContain('POST');
  expect(posts.filter((m) => m !== 'POST')).toEqual([]);
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

/**
 * Before the first refresh the device answers ready:false, and the console has
 * a dedicated branch for it. Nothing could reach that branch until the mock
 * grew a knob for it, so the message it prints was never once executed.
 */
test.describe('a panel that has not painted yet', () => {
  test.describe.configure({ mode: 'serial' });

  test.afterEach(async ({ page }) => {
    await scen(page, { panel_ready: 'true' });
  });

  test('says it is waiting rather than showing an empty frame', async ({ page }) => {
    await scen(page, { panel_ready: 'false' });
    await openConsole(page);
    await page.locator('#nav').click();
    await expect(page.locator('#pnlmsg')).toContainText(/waiting for the first refresh/i);
    // And it must NOT paint: a blank canvas at frame size would look like a
    // panel showing nothing, which is a different fault entirely.
    const w = await page.locator('#pnlcv').evaluate((c) => (c as HTMLCanvasElement).width);
    expect(w, 'it painted a frame the device said it did not have').not.toBe(500);
  });
});
