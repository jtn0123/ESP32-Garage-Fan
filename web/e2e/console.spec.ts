/**
 * The main screen: every control that is not behind the settings drawer.
 *
 * These run against the same mock concurrently, so nothing here may flip a
 * scenario knob -- knob-flipping lives in scenarios.spec.ts, which is serial.
 */
import { expect, hitBox, openConsole, recordRequests, test } from './harness';

test('loads, paints live data, and reports no errors', async ({ page }) => {
  await openConsole(page);
  await expect(page.locator('#brand')).toHaveText('GARAGE FAN');
  await expect(page.locator('#railnum')).not.toHaveText('–');
  // "Connecting to the controller…" must not survive a successful connection.
  await expect(page.locator('#reason')).not.toContainText('Connecting');
});

test('the status bar fills in from /api/state', async ({ page }) => {
  await openConsole(page);
  // The broker chip is on the header line at every width: whether the
  // controller is answering is the one header fact a phone still needs.
  await expect(page.locator('#hmq')).toContainText('BROKER');
});

test('the header carries firmware and battery on a wide screen', async ({ page }) => {
  await page.setViewportSize({ width: 1280, height: 720 });
  await openConsole(page);
  await expect(page.locator('#hfw')).not.toHaveText('FW —');
  await expect(page.locator('#hbat')).toBeVisible(); // mock reports a battery
});

/**
 * ...and files them in the diagnostics footer on a phone.
 *
 * The header was a single nowrap row that overflowed 320 px, so it now drops
 * these two at that width -- which is only acceptable because they have a home:
 * the footer strip that already carries SLOT, UPTIME and VBAT. Asserting the
 * home, not the hiding, is what keeps the information from quietly vanishing in
 * some later trim.
 */
test('a phone finds firmware and battery in the diagnostics footer', async ({ page }) => {
  await page.setViewportSize({ width: 320, height: 568 });
  await openConsole(page);
  await expect(page.locator('#hbits')).toBeHidden();
  const strip = page.locator('#stats');
  await expect(strip).toContainText('FW');
  await expect(strip).toContainText(/\d+\.\d+\.\d+/); // the running version
  await expect(strip).toContainText('VBAT');
  await expect(strip).toContainText(/\d+% · \d\.\d+ V/); // percentage AND volts
});

test('the hero shows garage, outside and differential', async ({ page }) => {
  await openConsole(page);
  for (const id of ['#tT', '#tO', '#tD']) {
    await expect(page.locator(id)).not.toHaveText('–');
  }
});

// ------------------------------------------------------------- the speed rail

test('the rail offers every running speed', async ({ page }) => {
  await openConsole(page);
  // 1..12. Zero is deliberately NOT a rail step -- it is the "Fan off" pill,
  // so stopping the fan cannot happen by a stray click at the end of the rail.
  await expect(page.locator('#stack button')).toHaveCount(12);
  await expect(page.locator('#stack button[title="set speed 1"]')).toBeVisible();
  await expect(page.locator('#stack button[title="set speed 12"]')).toBeVisible();
  await expect(page.locator('#stack button[title="set speed 0"]')).toHaveCount(0);
});

test('clicking a rail step commands that speed', async ({ page }) => {
  await openConsole(page);
  const posts = recordRequests(page, /\/api\/set/);
  // Index 0 is the TOP of the rail (speed 12) and the last is speed 0, so
  // assert on the title rather than position -- a reordered rail must not
  // silently start sending the wrong speed.
  await page.locator('#stack button[title="set speed 7"]').click();
  await expect(page.locator('#railnum')).toHaveText('7');
  // Polled, not read once: the DOM can settle before the browser has emitted
  // the request, so an immediate read is a race that fails on a loaded machine.
  await expect
    .poll(() => posts.some((r) => new URL(r.url()).searchParams.get('speed') === '7'), {
      message: 'the rail updated the display without commanding the fan',
    })
    .toBeTruthy();
});

test('both ends of the rail work', async ({ page }) => {
  await openConsole(page);
  await page.locator('#stack button[title="set speed 12"]').click();
  await expect(page.locator('#railnum')).toHaveText('12');
  await page.locator('#stack button[title="set speed 1"]').click();
  await expect(page.locator('#railnum')).toHaveText('1');
});

test('"Fan off" stops the fan and reads as off, not as a speed', async ({ page }) => {
  await openConsole(page);
  const posts = recordRequests(page, /\/api\/set/);
  await page.locator('#stack button[title="set speed 9"]').click();
  await expect(page.locator('#railnum')).toHaveText('9');
  await page.locator('#boff').click();
  // The readout says "off" rather than "0": a stopped fan should not look like
  // just another step on the dial.
  await expect(page.locator('#railnum')).toHaveText('off');
  // Parsed, not substring-matched: "speed=0" is also a prefix of "speed=07".
  await expect
    .poll(() => posts.some((r) => new URL(r.url()).searchParams.get('speed') === '0'))
    .toBeTruthy();
});

test('the auto pill toggles auto mode', async ({ page }) => {
  await openConsole(page);
  const posts = recordRequests(page, /\/api\/(set|config)/);
  const before = await page.locator('#bauto').getAttribute('class');
  await page.locator('#bauto').click();
  await expect
    .poll(() => page.locator('#bauto').getAttribute('class'))
    .not.toBe(before);
  // Same race as the rail: the pill can repaint before the request is emitted.
  await expect
    .poll(() => posts.length, { message: 'the pill changed but nothing was sent' })
    .toBeGreaterThan(0);
});

// ------------------------------------------------------------------ the scope

test('the PWM cell opens the waveform scope and closes again', async ({ page }) => {
  await openConsole(page);
  const scope = page.locator('#scope');
  await expect(scope).toHaveClass(/hide/);
  await page.locator('#pwmcell').click();
  await expect(scope).not.toHaveClass(/hide/);
  await expect(page.locator('#cv_pw')).toBeVisible();
  await page.locator('#scclose').click();
  await expect(scope).toHaveClass(/hide/);
});

test('the capture table offers all thirteen steps and transmits one', async ({ page }) => {
  await openConsole(page);
  await page.locator('#pwmcell').click();
  const cells = page.locator('#capgrid > *');
  await expect(cells).toHaveCount(13);
  const posts = recordRequests(page, /\/api\/set/);
  await cells.nth(4).click();
  await expect.poll(() => posts.length).toBeGreaterThan(0);
});

// ------------------------------------------------------------------ the drawer

test('SETTINGS opens the drawer and back returns to the console', async ({ page }) => {
  await openConsole(page);
  await expect(page.locator('#settings')).toHaveClass(/hide/);
  await page.locator('#nav').click();
  await expect(page.locator('#settings')).not.toHaveClass(/hide/);
  await expect(page.locator('#console')).toHaveClass(/hide/);
  await page.locator('#nav').click();
  await expect(page.locator('#settings')).toHaveClass(/hide/);
  await expect(page.locator('#console')).not.toHaveClass(/hide/);
});

// ------------------------------------------------------------------- fallbacks

/**
 * The device pushes state over SSE on port 8081; the mock does not serve it, so
 * EventSource fails here exactly as it does when that listener is down. The
 * page must keep tracking the fan off the 15 s poll -- api.ts swallows the
 * EventSource error precisely so this keeps working.
 *
 * The assertion has to be about a poll AFTER bootstrap. Counting requests from
 * page load proves only that the page loaded: a build where the SSE failure
 * killed the polling loop outright would still make that request once and pass.
 * So: take the count once the page is up, then require it to grow. The wait is
 * longer than the 15 s interval, which is why this is the slowest test here.
 */
test('state keeps polling when the SSE stream is unavailable', async ({ page }) => {
  await openConsole(page);
  const polls = recordRequests(page, /\/api\/state/);
  const atBootstrap = polls.length;
  await expect
    .poll(() => polls.length, {
      message: 'no /api/state poll after bootstrap: the fallback is not running',
      timeout: 25_000,
    })
    .toBeGreaterThan(atBootstrap);
});

test('the page stays interactive without SSE', async ({ page }) => {
  await openConsole(page);
  await page.locator('#stack button[title="set speed 3"]').click();
  await expect(page.locator('#railnum')).toHaveText('3');
});

/**
 * The narrowest phone still in service (iPhone SE, 320 CSS px).
 *
 * The header was one `white-space:nowrap` row measuring ~394 px there, so it
 * overflowed: the document became wider than the screen and the page panned
 * sideways, and SETTINGS sat past the right edge where nothing could reach it.
 * From the settings screen the same button is the ONLY way back, so opening
 * the drawer -- if you managed to -- was a one-way trip.
 */
test('every header control is reachable at 320 px', async ({ page }) => {
  await page.setViewportSize({ width: 320, height: 568 });
  await openConsole(page);

  const onScreen = async (sel: string): Promise<boolean> =>
    page.evaluate((s) => {
      const r = document.querySelector(s)?.getBoundingClientRect();
      const w = document.documentElement.clientWidth;
      return !!r && r.left >= 0 && r.right <= w && r.width > 0;
    }, sel);

  expect(await onScreen('#nav'), 'SETTINGS is off screen at 320 px').toBeTruthy();
  expect(await onScreen('#brand')).toBeTruthy();
  expect(await onScreen('#hmq')).toBeTruthy();
  // clientWidth, not innerWidth: it is the layout width in both projects
  // (mobile emulation keeps reporting the device's own innerWidth after a
  // resize, and a desk browser's scrollbar is not part of the page either).
  const overflow = await page.evaluate(
    () => document.documentElement.scrollWidth - document.documentElement.clientWidth,
  );
  expect(overflow, 'the page pans sideways at 320 px').toBeLessThanOrEqual(1);

  // The way back has to be reachable too, or the drawer is a trap.
  await page.locator('#nav').click();
  await expect(page.locator('#settings')).not.toHaveClass(/hide/);
  expect(await onScreen('#nav'), 'the return control is off screen at 320 px').toBeTruthy();
  await expect(page.locator('#nav')).toHaveText(/CONSOLE/);
  await page.locator('#nav').click();
  await expect(page.locator('#console')).not.toHaveClass(/hide/);
});

test('the page never scrolls sideways', async ({ page }) => {
  await openConsole(page);
  const overflow = await page.evaluate(
    () => document.documentElement.scrollWidth - document.documentElement.clientWidth,
  );
  expect(overflow, 'the layout overflows horizontally').toBeLessThanOrEqual(1);
});

// ------------------------------------------------------------- the speed rail

/**
 * The one control you use standing in a garage, and the one where a mis-tap
 * has a consequence: it changes the fan speed. Twelve 20x34 px blocks was
 * under half a fingertip.
 *
 * Twelve 44 px-WIDE targets cannot fit in 288 px, so the fix is the gesture:
 * the row is 44 px tall, a press previews the step under the finger, and the
 * command goes out once on release. What is asserted here is the hit box in
 * pixels rather than the CSS that produces it.
 */
test('the speed rail is a thumb-sized target', async ({ page }) => {
  // At phone width, where the rail is a row and a thumb is the input. On a desk
  // it is a narrow vertical column driven by a mouse, which is a different
  // control with different rules.
  await page.setViewportSize({ width: 320, height: 568 });
  await openConsole(page);
  const stack = await page.locator('#stack').boundingBox();
  expect(stack, '#stack has no box').not.toBeNull();
  expect(stack!.height, 'the rail is shorter than a fingertip').toBeGreaterThanOrEqual(44);
  const blocks = page.locator('#stack button');
  await expect(blocks).toHaveCount(12);
  for (const i of [0, 5, 11]) {
    const b = await blocks.nth(i).boundingBox();
    expect(b!.height, `block ${i} is under 44 px tall`).toBeGreaterThanOrEqual(44);
  }
});

test('sweeping the rail previews every step and commands once', async ({ page }) => {
  await openConsole(page);
  const sets = recordRequests(page, /\/api\/set/);
  const from = await page.locator('#stack button').nth(2).boundingBox();
  const to = await page.locator('#stack button').nth(7).boundingBox();
  if (!from || !to) return;
  // Centre to centre, so the same drag works on the phone's row and the desk's
  // bottom-up column without the test knowing which it is.
  await page.mouse.move(from.x + from.width / 2, from.y + from.height / 2);
  await page.mouse.down();
  await page.mouse.move(to.x + to.width / 2, to.y + to.height / 2, { steps: 10 });
  // Held, not released: the preview is the whole point of the gesture, and it
  // must show the pending step before anything is sent to the fan.
  await expect(page.locator('#railnum')).toHaveText('8');
  await expect(page.locator('#railnum')).toHaveClass(/pick/);
  expect(sets.length, 'the fan was commanded mid-sweep').toBe(0);

  await page.mouse.up();
  await expect.poll(() => sets.length).toBe(1);
  expect(sets[0]!.url()).toMatch(/speed=8\b/);
  await expect(page.locator('#railnum')).toHaveText('8');
  await expect(page.locator('#railnum')).not.toHaveClass(/pick/);
});

/**
 * The whole speed rail on the first screen of the smallest phone.
 *
 * This is the fold budget as an assertion rather than a claim in a report: at
 * 320x568 the rail used to start 19 px below the fold and end 52 px below it,
 * so the primary control needed a scroll. It now ends at ~546 with about 20 px
 * to spare, which is little enough that anything added above it needs to know.
 */
test('the speed rail is on the first screen at 320x568', async ({ page }) => {
  await page.setViewportSize({ width: 320, height: 568 });
  await openConsole(page);
  const geometry = await page.evaluate(() => {
    const bottom = (sel: string) => {
      const r = document.querySelector(sel)!.getBoundingClientRect();
      return +(r.bottom + scrollY).toFixed(1);
    };
    return { scrollY: window.scrollY, stack: bottom('#stack'), acts: bottom('#acts') };
  });
  expect(geometry.scrollY, 'the page was already scrolled').toBe(0);
  expect(geometry.acts, 'the auto/off pills are below the fold').toBeLessThanOrEqual(568);
  expect(geometry.stack, 'the speed rail is below the fold').toBeLessThanOrEqual(568);
});

/**
 * The controls the 44 px pass missed, measured as hit areas rather than boxes.
 *
 * `#nav` and `#scclose` keep a 13 px painted box deliberately -- padding on the
 * header would push the fold budget back down, and the scope bar is a
 * fixed-height strip -- so both grow their target with a transparent `::after`,
 * and only a probe can see that. The pills are the exception: they are a real
 * block in the flow and simply got taller.
 */
test('every touch control on the console screen is at least 44 px', async ({ page }) => {
  await page.setViewportSize({ width: 320, height: 568 });
  await openConsole(page);

  for (const sel of ['#nav', '#bauto', '#boff']) {
    const hit = await hitBox(page, sel);
    expect(hit.h, `${sel} hit area is ${hit.h} px tall`).toBeGreaterThanOrEqual(44);
    expect(hit.w, `${sel} hit area is ${hit.w} px wide`).toBeGreaterThanOrEqual(44);
  }

  // The scope's only dismissal, which needs the scope open to exist.
  await page.locator('#pwmcell').click();
  await expect(page.locator('#scope')).not.toHaveClass(/hide/);
  const close = await hitBox(page, '#scclose');
  expect(close.h, `#scclose hit area is ${close.h} px tall`).toBeGreaterThanOrEqual(44);
  expect(close.w, `#scclose hit area is ${close.w} px wide`).toBeGreaterThanOrEqual(44);
  await page.locator('#scclose').click();
  await expect(page.locator('#scope')).toHaveClass(/hide/);
});

/**
 * The scope's readouts have to be legible, not merely present.
 *
 * They shipped at #4a5a6e on near-black -- 2.5:1, against the 4.5:1 that 9 px
 * text needs, on a panel read on a phone in a garage. A colour is one token
 * away from regressing at any time and nothing else here would notice, so the
 * ratio is computed rather than eyeballed.
 *
 * #scdot is deliberately absent: it carries no text and no meaning of its own,
 * and the state it decorates is spelled out in #scstate beside it.
 */
test('the scope readouts meet the contrast minimum', async ({ page }) => {
  await openConsole(page);
  const ratios = await page.evaluate(() => {
    const channel = (v: number): number =>
      v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4);
    const lum = (rgb: string): number => {
      const [r, g, b] = rgb.match(/\d+(\.\d+)?/g)!.slice(0, 3).map(Number) as [
        number,
        number,
        number,
      ];
      return (
        0.2126 * channel(r / 255) + 0.7152 * channel(g / 255) + 0.0722 * channel(b / 255)
      );
    };
    // Walk up for the first ancestor that actually paints a background: the
    // readouts sit on the scope's gradient, not on their own fill.
    const backdrop = (el: Element): string => {
      for (let n: Element | null = el; n; n = n.parentElement) {
        const bg = getComputedStyle(n).backgroundColor;
        if (bg && !/rgba?\([^)]*,\s*0\s*\)/.test(bg) && bg !== 'transparent') return bg;
      }
      return 'rgb(0, 0, 0)';
    };
    return ['scstate', 'scknobs'].map((id) => {
      const el = document.getElementById(id)!;
      const a = lum(getComputedStyle(el).color);
      const b = lum(backdrop(el));
      const [hi, lo] = a > b ? [a, b] : [b, a];
      return { id, ratio: (hi + 0.05) / (lo + 0.05) };
    });
  });
  for (const { id, ratio } of ratios) {
    expect(ratio, `#${id} is ${ratio.toFixed(2)}:1 against its backdrop`).toBeGreaterThanOrEqual(
      4.5,
    );
  }
});

test('the page declares its language', async ({ page }) => {
  // Screen readers pick a pronunciation from this; without it they guess from
  // the system locale and read a console full of English abbreviations wrong.
  await openConsole(page);
  await expect(page.locator('html')).toHaveAttribute('lang', 'en');
});
