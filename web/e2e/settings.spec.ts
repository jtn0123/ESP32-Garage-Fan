/**
 * The settings drawer, including the two buttons that can destroy data.
 *
 * The maintenance actions are the reason this file exists. "Format SD card" and
 * "Delete card contents" are guarded by a token prompt AND a confirm, and the
 * only thing standing between a misclick and 28 GB is that both guards are
 * honoured. Nothing tested that before: the unit tests stub the API and the
 * contract tests never open a browser, so the wiring from button to request was
 * covered at neither end.
 */
import { expect, flushPageRequests, openConsole, recordRequests, test } from './harness';

async function openSettings(page: import('@playwright/test').Page): Promise<void> {
  await openConsole(page);
  await page.locator('#nav').click();
  await expect(page.locator('#settings')).not.toHaveClass(/hide/);
}

/**
 * Record dialog types as they arrive, answering each one as `reply` decides.
 *
 * The handler MUST be registered before the click. `window.prompt` blocks the
 * page, so an unhandled dialog leaves `click()` unresolved until the test times
 * out -- awaiting `waitForEvent('dialog')` after awaiting the click deadlocks
 * on exactly that. The returned array is the observable to synchronise on:
 * `expect.poll(() => seen.length)` waits for a real event instead of guessing
 * at a duration.
 */
function dialogLog(
  page: import('@playwright/test').Page,
  reply: (type: string) => 'accept' | 'dismiss',
): string[] {
  const seen: string[] = [];
  page.on('dialog', (d) => {
    seen.push(d.type());
    void (reply(d.type()) === 'accept' ? d.accept('any-token') : d.dismiss());
  });
  return seen;
}

test('every group renders', async ({ page }) => {
  await openSettings(page);
  for (const title of ['AUTO MODE', 'SENSORS', 'NETWORK', 'POWER', 'DEVICE', 'UPDATE']) {
    await expect(page.locator('#groups')).toContainText(title);
  }
});

test('the steppers push a change to the controller', async ({ page }) => {
  await openSettings(page);
  const posts = recordRequests(page, /\/api\/config/);
  // The first +/- pair in the drawer; whichever setting it is, pressing it must
  // reach the device. There is no save step, so a stepper that changes only the
  // label is a silent no-op.
  const plus = page.locator('#groups button', { hasText: '+' }).first();
  await plus.click();
  await expect.poll(() => posts.length).toBeGreaterThan(0);
});

test('a stepper moves its displayed value, and back again', async ({ page }) => {
  await openSettings(page);
  const stp = page.locator('#groups .stp').first();
  const value = stp.locator('span').first();
  const before = ((await value.textContent()) ?? '').trim();

  await stp.getByRole('button', { name: /^increase / }).click();
  await expect.poll(async () => ((await value.textContent()) ?? '').trim()).not.toBe(before);

  // Down again returns it, so the suite leaves the mock's config where it found
  // it -- these specs share one process with everything else in the run.
  await stp.getByRole('button', { name: /^decrease / }).click();
  await expect.poll(async () => ((await value.textContent()) ?? '').trim()).toBe(before);
});

test('the controls carry labels a screen reader can use', async ({ page }) => {
  await openSettings(page);
  // Steppers are a bare − and + with no text of their own, and toggles are an
  // empty box, so without these the drawer is unusable without sight.
  const steppers = page.locator('#groups .stp');
  const n = await steppers.count();
  expect(n, 'no steppers rendered').toBeGreaterThan(0);
  for (let i = 0; i < n; i++) {
    await expect(steppers.nth(i).getByRole('button', { name: /^decrease / })).toHaveCount(1);
    await expect(steppers.nth(i).getByRole('button', { name: /^increase / })).toHaveCount(1);
  }
  const toggles = page.locator('#groups button.tgl');
  for (let i = 0; i < (await toggles.count()); i++) {
    await expect(toggles.nth(i)).toHaveAttribute('role', 'switch');
    await expect(toggles.nth(i)).toHaveAttribute('aria-checked', /true|false/);
    await expect(toggles.nth(i)).toHaveAttribute('aria-label', /.+/);
  }
});

test('a toggle keeps aria-checked in step with what it shows', async ({ page }) => {
  await openSettings(page);
  const tgl = page.locator('#groups button.tgl').first();
  const before = await tgl.getAttribute('aria-checked');
  await tgl.click();
  await expect.poll(() => tgl.getAttribute('aria-checked')).not.toBe(before);
  // And the class the eye reads agrees with the attribute the reader reads.
  const checked = (await tgl.getAttribute('aria-checked')) === 'true';
  const looksOn = ((await tgl.getAttribute('class')) ?? '').includes('on');
  expect(looksOn, 'the toggle looks on but reports off (or vice versa)').toBe(checked);
  await tgl.click(); // leave it as found
});

test('toggles flip and report to the controller', async ({ page }) => {
  await openSettings(page);
  const tgl = page.locator('#groups button.tgl').first();
  const posts = recordRequests(page, /\/api\/(config|set)/);
  const before = await tgl.getAttribute('class');
  await tgl.click();
  await expect.poll(() => tgl.getAttribute('class')).not.toBe(before);
  await expect.poll(() => posts.length).toBeGreaterThan(0);
});

test('the gas boost controls reach the controller with their own keys', async ({ page }) => {
  await openSettings(page);
  const posts = recordRequests(page, /\/api\/config/);
  // The toggle names its key...
  const row = page.locator('#groups .grow', { hasText: 'Gas boost' }).first();
  await row.locator('button.tgl').click();
  await expect.poll(() => posts.some((r) => r.url().includes('gason='))).toBe(true);
  await row.locator('button.tgl').click(); // leave it as found
  // ...and so do both steppers. Without this the mock accepts anything and the
  // unit tests never click, so a typo'd key would 200 its way to production.
  const trig = page.locator('#groups .grow', { hasText: 'Gas boost · trigger' });
  await trig.locator('button').last().click();
  await expect.poll(() => posts.some((r) => r.url().includes('gasvoc='))).toBe(true);
  const spd = page.locator('#groups .grow', { hasText: 'Gas boost · speed' });
  await spd.locator('button').last().click();
  await expect.poll(() => posts.some((r) => r.url().includes('gasspd='))).toBe(true);
});

test('the electricity price stepper reaches the controller as ckwh', async ({ page }) => {
  await openSettings(page);
  const posts = recordRequests(page, /\/api\/config/);
  const row = page.locator('#groups .grow', { hasText: 'Electricity price' });
  await row.locator('button').last().click();
  await expect.poll(() => posts.some((r) => r.url().includes('ckwh='))).toBe(true);
  await row.locator('button').first().click(); // leave the price as found
});

test('the CSV download exports the range the chart is showing', async ({ page }) => {
  // It used to be hardcoded at 30 days while the chart offered 60, so the
  // data you could SEE was not the data you could TAKE.
  await openConsole(page);
  await page.locator('#ranges button[data-d="60"]').click();
  await page.locator('#nav').click();
  await expect(page.locator('#settings')).not.toHaveClass(/hide/);
  const link = page.locator('#groups a', { hasText: /Download CSV/i });
  await expect(link).toHaveAttribute('href', /\/download\.csv\?days=60/);
  await expect(link).toHaveText(/60 d/);
});

// ------------------------------------------------------- the dangerous pair

test('the destructive actions are marked as such', async ({ page }) => {
  await openSettings(page);
  await expect(page.locator('#groups button.danger', { hasText: /Delete card contents/i })).toHaveCount(1);
  await expect(page.locator('#groups button.danger', { hasText: /Format SD card/i })).toHaveCount(1);
  // Restart reboots but destroys nothing, so it must NOT wear the danger style
  // -- if everything is red, nothing is.
  await expect(page.locator('#groups button.danger', { hasText: /^Restart$/ })).toHaveCount(0);
});

for (const label of [/Format SD card/i, /Delete card contents/i, /^Restart$/]) {
  test(`dismissing the token prompt for ${label.source} sends nothing`, async ({ page }) => {
    await openSettings(page);
    const posts = recordRequests(page, /\/api\/(sdformat|sdpurge|restart)/);
    const seen = dialogLog(page, () => 'dismiss');
    await page.locator('#groups button', { hasText: label }).first().click();
    await expect.poll(() => seen.length).toBeGreaterThan(0);
    expect(seen[0], 'the first guard should be the token prompt').toBe('prompt');
    await flushPageRequests(page);
    expect(posts, 'a cancelled prompt still hit the device').toHaveLength(0);
  });

  test(`declining the confirmation for ${label.source} sends nothing`, async ({ page }) => {
    await openSettings(page);
    const posts = recordRequests(page, /\/api\/(sdformat|sdpurge|restart)/);
    // Supply a token, then say no at the confirm. This is the misclick case:
    // the guard that actually protects the card.
    const seen = dialogLog(page, (t) => (t === 'prompt' ? 'accept' : 'dismiss'));
    await page.locator('#groups button', { hasText: label }).first().click();
    await expect.poll(() => seen.length).toBeGreaterThanOrEqual(2);
    expect(seen.slice(0, 2), 'a token alone should not be enough to proceed').toEqual([
      'prompt',
      'confirm',
    ]);
    await flushPageRequests(page);
    expect(posts, 'declining the confirmation still hit the device').toHaveLength(0);
  });
}

test('confirming a purge does reach the device', async ({ page }) => {
  await openSettings(page);
  const posts = recordRequests(page, /\/api\/sdpurge/);
  const dialogs: string[] = [];
  page.on('dialog', (d) => {
    dialogs.push(d.type());
    return void d.accept(d.type() === 'prompt' ? 'any-token' : '');
  });
  await page.locator('#groups button', { hasText: /Delete card contents/i }).first().click();
  await expect.poll(() => posts.length).toBeGreaterThan(0);
  // Both guards were asked, in order, before anything was sent.
  expect(dialogs.slice(0, 2)).toEqual(['prompt', 'confirm']);
  // The mock refuses the token, so the console must surface the failure rather
  // than claim success.
  await expect.poll(() => dialogs.length).toBeGreaterThanOrEqual(3);
});

test('a purge is POSTed, never GETed', async ({ page }) => {
  await openSettings(page);
  const posts = recordRequests(page, /\/api\/sdpurge/);
  page.on('dialog', (d) => void d.accept(d.type() === 'prompt' ? 'any-token' : ''));
  await page.locator('#groups button', { hasText: /Delete card contents/i }).first().click();
  await expect.poll(() => posts.length).toBeGreaterThan(0);
  // A GET that wipes a card is reachable from a link, a prefetch or a crawler.
  expect(posts.map((r) => r.method())).not.toContain('GET');
});

// -------------------------------------------------------------- update / OTA

test('the update section offers a re-check', async ({ page }) => {
  await openSettings(page);
  await expect(page.locator('#groups button.updbtn', { hasText: /Check again/i })).toHaveCount(1);
});

test('the OTA form is present with a token field and a file picker', async ({ page }) => {
  await openSettings(page);
  await expect(page.locator('#ota_t')).toBeVisible();
  await expect(page.locator('#ota_f')).toBeAttached();
  await expect(page.locator('#ota_go')).toBeVisible();
});

test('uploading without picking a file is refused, not sent', async ({ page }) => {
  await openSettings(page);
  const uploads = recordRequests(page, /\/update/);
  await page.locator('#ota_t').fill('any-token');
  await page.locator('#ota_go').click();
  await expect(page.locator('#otamsg')).not.toBeEmpty();
  expect(uploads, 'an empty upload was sent to /update').toHaveLength(0);
});

test('the token field is used instead of prompting', async ({ page }) => {
  await openSettings(page);
  await page.locator('#ota_t').fill('token-from-the-field');

  // Positive assertion rather than "no prompt appeared": with the field filled
  // the FIRST dialog must be the confirmation, because the token step was
  // answered without asking. Waiting a fixed moment and checking a flag proves
  // nothing about what would have happened a millisecond later.
  const posts = recordRequests(page, /\/api\/restart/);
  const seen = dialogLog(page, () => 'accept');
  await page.locator('#groups button', { hasText: /^Restart$/ }).first().click();
  await expect.poll(() => seen.length).toBeGreaterThan(0);
  expect(seen[0], 'it prompted even though the token field was filled').toBe('confirm');

  // Then follow through: not prompting is only half the claim. The value in the
  // field has to be the value that reaches the device, or the field is
  // decorative and the operator's token silently never arrives.
  await expect.poll(() => posts.length).toBeGreaterThan(0);
  const req = posts[0];
  expect(req?.method()).toBe('POST');
  expect(req?.url()).toContain('token=token-from-the-field');
});

/**
 * Every control in the drawer, measured rather than declared.
 *
 * These were 26-30 px tall -- including `Format SD card` and `Delete card
 * contents`, which erase the flight recorder. A destructive button smaller
 * than a fingertip is a design bug with consequences.
 */
test('every settings control is at least 44 px tall', async ({ page }) => {
  await page.setViewportSize({ width: 320, height: 568 });
  await openConsole(page);
  await page.locator('#nav').click();
  await expect(page.locator('#settings')).not.toHaveClass(/hide/);

  const small = await page.evaluate(() => {
    const out: string[] = [];
    const seen = document.querySelectorAll<HTMLElement>(
      '#groups .stp button, #groups .stp span, #groups .acts button, #groups .acts a, #groups .tgl',
    );
    for (const el of seen) {
      const r = el.getBoundingClientRect();
      if (r.width === 0 && r.height === 0) continue; // inside a collapsed row
      // The switch keeps its 42x24 pill and grows only its hit area, so measure
      // what a finger actually hits, not what is painted.
      const hit = el.classList.contains('tgl') ? r.height + 20 : r.height;
      if (hit < 44) out.push(`${el.className || el.tagName}: ${hit.toFixed(1)}px`);
    }
    return out;
  });
  expect(small, `controls under 44 px: ${small.join(', ')}`).toEqual([]);
  expect(await page.locator('#groups .acts button').count()).toBeGreaterThan(0);
});
