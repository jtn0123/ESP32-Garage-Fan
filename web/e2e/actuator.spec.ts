import { expect, openConsole, resetScen, scen, test } from './harness';

test.afterEach(async ({ page }) => { await resetScen(page); });

test('a rejected PWM command shows a fault and fails health monitoring', async ({ page }) => {
  await scen(page, { actuator_fault: 'true' });
  await openConsole(page);
  await expect(page.locator('#pwmval')).toHaveText('FAULT');
  await expect(page.locator('#pwmhigh')).toHaveText(/PWM command rejected/);
  const health = await page.request.get('/health');
  expect(health.status()).toBe(503);
  expect((await health.json()).actuator_ok).toBe(false);
});
