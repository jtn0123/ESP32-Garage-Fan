/**
 * The connection form: the board's own credentials, edited from the page.
 *
 * This is the prerequisite for a published release being installable, so the
 * wiring from form to /api/provision is worth the same rigour as the
 * destructive buttons: the token guard, the confirm, the exact query that
 * leaves, and what the operator is told afterwards.
 */
import { expect, openConsole, recordRequests, test } from './harness';

async function openSettings(page: import('@playwright/test').Page): Promise<void> {
  await openConsole(page);
  await page.locator('#nav').click();
  await expect(page.locator('#settings')).not.toHaveClass(/hide/);
}

test('the form pre-fills from the device and never shows a password', async ({ page }) => {
  await openSettings(page);
  await expect(page.locator('#prov_ssid')).toHaveValue('example-wifi');
  await expect(page.locator('#prov_mqtt_host')).toHaveValue('192.0.2.10');
  await expect(page.locator('#prov_mqtt_port')).toHaveValue('1883');
  await expect(page.locator('#prov_mqtt_user')).toHaveValue('fan');
  await expect(page.locator('#prov_pass')).toHaveValue('');
  await expect(page.locator('#prov_mqtt_pass')).toHaveValue('');
});

test('saving with nothing changed sends nothing and says so', async ({ page }) => {
  await openSettings(page);
  const posts = recordRequests(page, /\/api\/provision/);
  await page.locator('#prov_go').click();
  await expect(page.locator('#provmsg')).toHaveText(/nothing changed/);
  expect(posts).toHaveLength(0);
});

test('a changed field goes out with the token, and only that field', async ({ page }) => {
  await openSettings(page);
  const posts = recordRequests(page, /\/api\/provision/);
  // Token from the prompt, then accept the reboot confirm.
  page.on('dialog', (d) => void d.accept(d.type() === 'prompt' ? 'example-update-token' : ''));
  await page.locator('#prov_mqtt_user').fill('fan-new');
  await page.locator('#prov_go').click();
  await expect(page.locator('#provmsg')).toHaveText(/rebooting onto the new settings/);
  expect(posts).toHaveLength(1);
  const url = new URL(posts[0]!.url());
  expect(url.search).toBe('');
  expect(posts[0]!.headers()['x-fan-token']).toBe('example-update-token');
  expect(new URLSearchParams(posts[0]!.postData()!).get('mqtt_user')).toBe('fan-new');
  // Put the mock back for the neighbours sharing this worker.
  await page.request.post(`${url.origin}/api/provision`, {
    headers: { 'X-Fan-Token': 'example-update-token' }, form: { mqtt_user: 'fan' },
  });
});

test('a wrong token is refused and the form says why', async ({ page }) => {
  await openSettings(page);
  page.on('dialog', (d) => void d.accept(d.type() === 'prompt' ? 'wrong' : ''));
  await page.locator('#prov_lat').fill('45.5');
  await page.locator('#prov_go').click();
  await expect(page.locator('#provmsg')).toHaveText(/refused: bad token/);
});

test('an invalid port is stopped before it reaches the device', async ({ page }) => {
  await openSettings(page);
  const posts = recordRequests(page, /\/api\/provision/);
  await page.locator('#prov_mqtt_port').fill('99999');
  await page.locator('#prov_go').click();
  await expect(page.locator('#provmsg')).toHaveText(/port/);
  expect(posts).toHaveLength(0);
});

test('password whitespace survives the form without exposing credentials in URLs', async ({ page }) => {
  await openSettings(page);
  const posts = recordRequests(page, /\/api\/provision/);
  page.on('dialog', (d) => void d.accept(d.type() === 'prompt' ? 'example-update-token' : ''));
  await page.locator('#prov_pass').fill(' wifi ');
  await page.locator('#prov_mqtt_pass').fill(' mqtt ');
  await page.locator('#prov_go').click();
  await expect(page.locator('#provmsg')).toHaveText(/rebooting onto the new settings/);
  expect(posts).toHaveLength(1);
  expect(new URL(posts[0]!.url()).search).toBe('');
  const body = new URLSearchParams(posts[0]!.postData()!);
  expect(body.get('pass')).toBe(' wifi ');
  expect(body.get('mqtt_pass')).toBe(' mqtt ');
});
