/**
 * One-click install, end to end: the release feed says a newer version
 * exists, the update channel (GitHub Pages) serves the image and its
 * checksum, and the console downloads, verifies, uploads and waits for the
 * board to confirm. Both remote origins are routed to fixtures here -- the
 * suite must not depend on GitHub -- and the mock's ota_fw knob decides what
 * the "rebooted" board reports.
 */
import { createHash } from 'node:crypto';

import { expect, hitBox, openConsole, recordRequests, resetScen, scen, test } from './harness';

// These specs flip knobs (ota_fw) and an accepted /update moves the mock's
// reported fw, boots and slot. Put the worker's mock back after each one, or
// the next test in this worker finds a board already "running" v9.9.9 and the
// update row says up to date -- no button, 30 s of waiting for it.
test.afterEach(async ({ page }) => {
  await resetScen(page);
});

const TAG = 'v9.9.9';
const NAME = `garage-fan-${TAG}.bin`;
const IMAGE = Buffer.from('FAKE-FIRMWARE-IMAGE-BYTES');
const REPO = 'jtn0123/ESP32-Garage-Fan';
const CHANNEL = `https://jtn0123.github.io/ESP32-Garage-Fan/firmware/${NAME}`;

async function routeChannel(
  page: import('@playwright/test').Page,
  opts: { checksumOk?: boolean } = {},
): Promise<void> {
  await page.route('https://api.github.com/repos/**', (r) =>
    r.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify([
        {
          tag_name: TAG,
          name: `Release ${TAG}`,
          html_url: `https://github.com/${REPO}/releases/tag/${TAG}`,
          body: 'notes',
          draft: false,
          prerelease: false,
          assets: [
            {
              name: NAME,
              browser_download_url: `https://github.com/${REPO}/releases/download/${TAG}/${NAME}`,
              size: IMAGE.length,
            },
          ],
        },
      ]),
    }),
  );
  await page.route(CHANNEL, (r) =>
    r.fulfill({ status: 200, contentType: 'application/octet-stream', body: IMAGE }),
  );
  const sum = opts.checksumOk === false ? '0'.repeat(64) : createHash('sha256').update(IMAGE).digest('hex');
  await page.route(`${CHANNEL}.sha256`, (r) =>
    r.fulfill({ status: 200, contentType: 'text/plain', body: `${sum}  ${NAME}\n` }),
  );
}

async function openSettings(page: import('@playwright/test').Page): Promise<void> {
  await openConsole(page);
  await page.locator('#nav').click();
  await expect(page.locator('#settings')).not.toHaveClass(/hide/);
}

test('the header UPDATE flag says so in words and opens the update group', async ({ page }) => {
  await routeChannel(page);
  await page.setViewportSize({ width: 320, height: 568 });
  await openConsole(page);
  const flag = page.locator('#updot');
  await expect(flag).toHaveText('UPDATE');
  const hit = await hitBox(page, '#updot');
  expect(hit.h, `#updot hit area is ${hit.h} px tall`).toBeGreaterThanOrEqual(44);
  expect(hit.w, `#updot hit area is ${hit.w} px wide`).toBeGreaterThanOrEqual(44);
  // On a 320 px phone it stands in for SETTINGS rather than wrap the header
  // onto a second line, which would push the speed rail below the fold.
  await expect(page.locator('#nav')).toBeHidden();
  const stack = await page.locator('#stack').boundingBox();
  expect(stack!.y + stack!.height, 'the speed rail is below the fold').toBeLessThanOrEqual(568);
  await flag.click();
  await expect(page.locator('#settings')).not.toHaveClass(/hide/);
  await expect(page.locator('#groups .gt', { hasText: /^UPDATE$/ })).toBeInViewport();
  // Settings already says it; the way back is the nav again.
  await expect(flag).toBeHidden();
  await expect(page.locator('#nav')).toHaveText('← CONSOLE');
});

test('one click installs the published release and waits for confirmation', async ({ page }) => {
  await routeChannel(page);
  await scen(page, { ota_fw: '9.9.9' });
  await openSettings(page);
  const uploads = recordRequests(page, /\/update(?:\?|$)/);
  await expect(page.locator('#upd_go')).toBeVisible();
  page.on('dialog', (d) => void d.accept('example-update-token'));
  await page.locator('#upd_go').click();
  await expect(page.locator('#updmsg')).toHaveText(/updated to v9.9.9 and confirmed/, { timeout: 20_000 });
  expect(uploads).toHaveLength(1);
  expect(new URL(uploads[0]!.url()).search).toBe('');
  expect(uploads[0]!.headers()['x-fan-token']).toBe('example-update-token');
});

test('a checksum mismatch aborts before anything reaches the controller', async ({ page }) => {
  await routeChannel(page, { checksumOk: false });
  await openSettings(page);
  const uploads = recordRequests(page, /\/update(?:\?|$)/);
  page.on('dialog', (d) => void d.accept('example-update-token'));
  await page.locator('#upd_go').click();
  await expect(page.locator('#updmsg')).toHaveText(/ABORTED.*checksum/, { timeout: 15_000 });
  expect(uploads).toHaveLength(0);
});

test('a board that comes back on the old version is called a rollback', async ({ page }) => {
  await routeChannel(page); // ota_fw stays none: the mock "reboots" onto the same fw
  await openSettings(page);
  page.on('dialog', (d) => void d.accept('example-update-token'));
  await page.locator('#upd_go').click();
  await expect(page.locator('#updmsg')).toHaveText(/rolled back/, { timeout: 20_000 });
});

test('the token field, when filled, is what travels', async ({ page }) => {
  await routeChannel(page);
  await openSettings(page);
  page.on('dialog', (d) => void d.dismiss()); // the retry prompt: decline it
  await page.locator('#ota_t').fill('not-the-token');
  await page.locator('#upd_go').click();
  await expect(page.locator('#updmsg')).toHaveText(/refused the update token/, { timeout: 15_000 });
});
