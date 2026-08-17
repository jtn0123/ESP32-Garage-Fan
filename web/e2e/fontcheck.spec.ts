import { expect, openConsole, test } from './harness';
test('fontcheck', async ({ page }) => {
  await openConsole(page);
  const m = await page.evaluate(() => {
    const probe = document.createElement('span');
    probe.style.cssText = 'position:absolute;font:62px "Instrument Sans",sans-serif;white-space:pre';
    probe.textContent = '74.9';
    document.body.append(probe);
    const sansW = +probe.getBoundingClientRect().width.toFixed(2);
    probe.remove();
    return { sansW, tT: +document.getElementById('tT')!.getBoundingClientRect().width.toFixed(2),
      linkMedia: document.querySelector('link[rel=stylesheet][onload]')?.media ?? 'none' };
  });
  console.log('FONTPROBE', JSON.stringify(m));
  expect(true).toBe(true);
});
