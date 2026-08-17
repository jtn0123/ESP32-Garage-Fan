import { expect, openConsole, test } from './harness';
test('fontcheck', async ({ page }) => {
  await openConsole(page);
  const m = await page.evaluate(() => {
    const w = (s: string) => +document.querySelector(s)!.getBoundingClientRect().width.toFixed(2);
    const probe = document.createElement('span');
    probe.style.cssText = 'position:absolute;font:11px "JetBrains Mono",monospace;white-space:pre';
    probe.textContent = '0000000000';
    document.body.append(probe);
    const monoW = +probe.getBoundingClientRect().width.toFixed(2);
    probe.style.font = '62px "Instrument Sans",sans-serif';
    probe.textContent = '74.9';
    const sansW = +probe.getBoundingClientRect().width.toFixed(2);
    probe.remove();
    return { brand: w('#brand'), tT: w('#tT'), stamp: w('#stamp'), monoW, sansW,
      linkMedia: document.querySelector('link[rel=stylesheet][onload]')?.media ?? 'none' };
  });
  console.log('FONTPROBE', JSON.stringify(m));
  expect(true).toBe(true);
});
