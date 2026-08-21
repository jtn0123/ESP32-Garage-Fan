/**
 * The live stream, previously the one transport with zero coverage: the mock
 * now serves SSE one port above the console (the device's 80/8081 split,
 * shrunk to PORT/PORT+1 — api.ts derives the offset from location.port), and
 * these prove the page actually listens to it.
 *
 * The state poll also repaints and runs every 15 s, close enough to these
 * timeouts to pass a broken stream by luck. So each spec first cuts the poll
 * off at the route layer; after that, only a frame from the stream can move
 * the page. ("Failed to load resource" noise from the aborted polls is
 * already tolerated by the harness error guard — offline scenarios rely on
 * the same allowance.)
 */
import { expect, openConsole, test } from './harness';

test('a speed change lands over the live stream, not the poll', async ({
  page,
  baseURL,
  request,
}) => {
  await openConsole(page);
  await page.route('**/api/state', (r) => r.abort()); // the poll is now blind
  const before = (await page.locator('#railnum').textContent())?.trim() ?? '9';
  const target = before === '4' ? '5' : '4';
  // Mutate the device behind the page's back. No Origin header on purpose:
  // a non-browser caller is accepted, same as the firmware's own rule.
  const res = await request.post(`${baseURL}/api/set?speed=${target}`);
  expect(res.ok()).toBeTruthy();
  // Frames arrive every second; only the stream can repaint this readout now.
  await expect(page.locator('#railnum')).toHaveText(target, { timeout: 8000 });
  await request.post(`${baseURL}/api/set?speed=${before}`); // neighbours share this mock
});
