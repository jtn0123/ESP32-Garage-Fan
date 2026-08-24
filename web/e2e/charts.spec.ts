/**
 * The history area: range switching, the optional rows, and scrubbing.
 *
 * The range buttons are worth real cover because they were wrong on the device
 * twice: /api/history?days= used to default to 1 for a typo'd query (serving
 * RAM-ring data as if it were the card's), and the 7/30-day branch used to
 * return three series instead of seven.
 */
// Touch gestures live in touch.spec.ts: they need a touchscreen, which the
// desk project does not have.
import { expect, inkedColumns, openConsole, openTip, recordRequests, test } from './harness';

test('the six ranges are offered and 24H starts selected', async ({ page }) => {
  await openConsole(page);
  await expect(page.locator('#ranges button')).toHaveCount(6);
  await expect(page.locator('#ranges button[data-d="1"]')).toHaveClass(/on/);
  await expect(page.locator('#chtitle')).toHaveText(/24 HOURS/i);
});

for (const [days, title] of [
  ['7', /7 DAYS/i],
  ['30', /30 DAYS/i],
  ['60', /60 DAYS/i],
  ['1', /24 HOURS/i],
] as const) {
  test(`selecting ${days}d refetches that range and relabels the chart`, async ({ page }) => {
    await openConsole(page);
    const fetches = recordRequests(page, /\/api\/history/);
    await page.locator(`#ranges button[data-d="${days}"]`).click();
    await expect(page.locator(`#ranges button[data-d="${days}"]`)).toHaveClass(/on/);
    await expect(page.locator('#chtitle')).toHaveText(title);
    // The request must name the range asked for. A range that quietly serves
    // another window is the exact defect this endpoint already shipped once.
    await expect
      .poll(() => fetches.some((r) => new RegExp(`days=${days}\\b`).test(r.url())))
      .toBeTruthy();
  });
}

/**
 * The sub-day ranges are a WINDOW on the 24 h response, not a query of their
 * own -- the firmware answers days=1|7|30|60 and 400s anything else, on
 * purpose. So the thing worth pinning is the REQUEST: a 6H button sending
 * days=0.25 would take a 400 and draw nothing, and one sending days=6 would
 * silently draw six DAYS under a six-hour title. Neither is visible to a unit
 * test, which never clicks anything. The slicing maths is covered in
 * tests/series.test.ts.
 */
for (const [d, hours] of [
  ['0.25', 6],
  ['0.5', 12],
] as const) {
  test(`the ${hours}H range windows the 24 h response instead of asking for it`, async ({
    page,
  }) => {
    await openConsole(page);
    const fetches = recordRequests(page, /\/api\/history/);
    await page.locator(`#ranges button[data-d="${d}"]`).click();
    await expect(page.locator(`#ranges button[data-d="${d}"]`)).toHaveClass(/on/);
    await expect(page.locator('#chtitle')).toHaveText(new RegExp(`${hours} HOURS`, 'i'));
    await expect(page.locator('#ranges button.on')).toHaveCount(1);
    await expect.poll(() => fetches.some((r) => /days=1\b/.test(r.url()))).toBeTruthy();
    // Never the fraction, and never the hour count read as days.
    expect(fetches.some((r) => /days=(0\.|6\b|12\b)/.test(r.url()))).toBe(false);
  });

  test(`the ${hours}H range actually plots its data`, async ({ page }) => {
    await openConsole(page);
    await page.locator(`#ranges button[data-d="${d}"]`).click();
    await expect(page.locator('#chtitle')).toHaveText(new RegExp(`${hours} HOURS`, 'i'));
    await expect
      .poll(() => inkedColumns(page, 'cv_t'), {
        message: `the ${hours}-hour chart is blank`,
        timeout: 15_000,
      })
      .toBeGreaterThan(100);
    await expect(page.locator('#roT')).toContainText('°');
    await expect(page.locator('#tcap')).not.toContainText(/could not|no samples/i);
  });
}

test('leaving a sub-day range for a wide one asks the device again', async ({ page }) => {
  // 6H is served by slicing the 24 h payload; 7D is not, and the switch has to
  // go back to the network rather than window what is already in hand.
  await openConsole(page);
  await page.locator('#ranges button[data-d="0.25"]').click();
  await expect(page.locator('#chtitle')).toHaveText(/6 HOURS/i);
  const fetches = recordRequests(page, /\/api\/history/);
  await page.locator('#ranges button[data-d="7"]').click();
  await expect(page.locator('#chtitle')).toHaveText(/7 DAYS/i);
  await expect.poll(() => fetches.some((r) => /days=7\b/.test(r.url()))).toBeTruthy();
});

/**
 * A relabelled chart is not a plotted one.
 *
 * Every assertion above this was satisfied by a 7D button that fetched, said
 * "LAST 7 DAYS", and drew nothing -- which is exactly what the console did:
 * the wide ranges arrive decimated (~2400 s per row at 7 days against the
 * device's 300 s cadence), and measuring that step against interval_s flagged
 * every row as an outage, so every line broke into invisible one-point
 * segments. Ink on the canvas and a filled readout are what distinguish a
 * range that works from one that only announces itself.
 */
for (const days of ['7', '30', '60'] as const) {
  test(`the ${days}-day range actually plots its data`, async ({ page }) => {
    await openConsole(page);
    await page.locator(`#ranges button[data-d="${days}"]`).click();
    await expect(page.locator('#chtitle')).toHaveText(new RegExp(`${days} DAYS`, 'i'));
    await expect
      .poll(() => inkedColumns(page, 'cv_t'), {
        message: `the ${days}-day chart is blank`,
        timeout: 15_000,
      })
      .toBeGreaterThan(100);
    await expect(page.locator('#roT')).toContainText('°');
    // And nothing apologises: a working range needs no explanation.
    await expect(page.locator('#tcap')).not.toContainText(/could not|no samples/i);
  });
}

test('only one range is selected at a time', async ({ page }) => {
  await openConsole(page);
  await page.locator('#ranges button[data-d="7"]').click();
  await expect(page.locator('#ranges button[data-d="7"]')).toHaveClass(/on/);
  await expect(page.locator('#ranges button.on')).toHaveCount(1);
});

test('the temperature chart always draws', async ({ page }) => {
  await openConsole(page);
  const painted = await page.evaluate(() => {
    const c = document.getElementById('cv_t') as HTMLCanvasElement | null;
    return !!c && c.width > 0 && c.height > 0;
  });
  expect(painted, '#cv_t has no drawing surface').toBeTruthy();
});

// ------------------------------------------------------------- the extra rows

// Each chip owns exactly one row. Asserting the MAPPING, not just "something
// appeared", is what catches a chip wired to the wrong series.
const CHIP_ROWS = [
  { key: 'fan', row: '#sub_s' },
  { key: 'humidity', row: '#sub_h' },
  { key: 'pressure', row: '#sub_p' },
  { key: 'battery', row: '#sub_b' },
  { key: 'power', row: '#sub_w' },
  { key: 'voc', row: '#sub_v' },
  { key: 'nox', row: '#sub_n' },
] as const;

test('every series has a chip', async ({ page }) => {
  await openConsole(page);
  await expect(page.locator('#chips button')).toHaveCount(CHIP_ROWS.length);
  for (const { key } of CHIP_ROWS) {
    await expect(page.locator(`#chips button[data-key="${key}"]`)).toBeVisible();
  }
});

for (const { key, row } of CHIP_ROWS) {
  test(`the ${key} chip toggles ${row} and nothing else`, async ({ page }) => {
    await openConsole(page);
    const chip = page.locator(`#chips button[data-key="${key}"]`);
    const target = page.locator(row);
    const others = CHIP_ROWS.filter((c) => c.row !== row).map((c) => c.row);
    const otherState = async () =>
      Promise.all(others.map((sel) => page.locator(sel).getAttribute('class')));

    // FAN starts shown and the rest start hidden, so read the state rather than
    // assuming it -- the test has to work whichever way the default goes.
    const shownBefore = !((await target.getAttribute('class')) ?? '').includes('hide');
    const before = await otherState();

    await chip.click();
    await expect(target).toHaveClass(shownBefore ? /hide/ : /^(?!.*hide).*$/);
    expect(await otherState(), `toggling ${key} disturbed another row`).toEqual(before);

    await chip.click();
    await expect(target).toHaveClass(shownBefore ? /^(?!.*hide).*$/ : /hide/);
  });
}

// ---------------------------------------------------------------- scrubbing

/**
 * Scrubbing repaints the HERO, not a tooltip: the big reading becomes the value
 * at the moment under the cursor and the stamp stops saying NOW. That is the
 * whole feature -- "drag across to read any moment", as the caption promises.
 */
test('dragging across the plots reads out a past moment', async ({ page }) => {
  await openConsole(page);
  const plots = page.locator('#plots');
  await expect(plots).toBeVisible();
  await expect(page.locator('#stamp')).toHaveText('NOW');
  const box = await plots.boundingBox();
  expect(box).not.toBeNull();
  if (!box) return;
  await page.mouse.move(box.x + box.width * 0.5, box.y + box.height * 0.4);
  await expect(page.locator('#stamp')).not.toHaveText('NOW');
});

test('leaving the plots returns the hero to now', async ({ page }) => {
  await openConsole(page);
  const box = await page.locator('#plots').boundingBox();
  if (!box) return;
  await page.mouse.move(box.x + box.width * 0.5, box.y + box.height * 0.4);
  await expect(page.locator('#stamp')).not.toHaveText('NOW');
  // Out of the plots entirely, which is what fires mouseleave.
  await page.mouse.move(box.x - 60, box.y - 60);
  await expect(page.locator('#stamp')).toHaveText('NOW');
});

/**
 * The moment being pointed at has to be READABLE, and not only under the
 * finger. On a phone the hero stamp has scrolled off the top by the time a
 * thumb reaches the plots and the hand covers the crosshair, so the chart
 * header carries it too -- that is the only copy a touch user can see.
 */
test('scrubbing names the moment above the plots', async ({ page }) => {
  await openConsole(page);
  await expect(page.locator('#chtitle')).toHaveText(/24 HOURS/i);
  const box = await page.locator('#cv_t').boundingBox();
  if (!box) return;
  await page.mouse.move(box.x + box.width * 0.5, box.y + box.height * 0.5);
  // A clock, not merely "not NOW": a scrub readout with no time answers nothing.
  await expect(page.locator('#chtitle')).toHaveText(/\d{1,2}:\d{2}/);
  await expect(page.locator('#stamp')).toHaveText(/\d{1,2}:\d{2}/);
  // And the per-row readout tracks the same sample.
  await expect(page.locator('#roT')).toContainText('°');
});

test('a scrubbed moment on a multi-day range carries its date', async ({ page }) => {
  await openConsole(page);
  await page.locator('#ranges button[data-d="7"]').click();
  await expect(page.locator('#chtitle')).toHaveText(/7 DAYS/i);
  const box = await page.locator('#cv_t').boundingBox();
  if (!box) return;
  await page.mouse.move(box.x + box.width * 0.4, box.y + box.height * 0.5);
  // "15:27" happens once a day for a week; the day is the point of the range.
  await expect(page.locator('#chtitle')).toHaveText(/\d{1,2}\/\d{1,2} \d{1,2}:\d{2}/);
});

test('a status bit toggles on click', async ({ page }) => {
  await openConsole(page);
  const bit = page.locator('#stats .bit').first();
  const hidden = async () =>
    ((await page.locator('#tip').getAttribute('class')) ?? '').includes('hide');

  await bit.click();
  const afterFirst = await hidden();
  await bit.click();
  expect(await hidden(), 'a second click did not toggle the tip back').toBe(!afterFirst);
  await bit.click();
  expect(await hidden()).toBe(afterFirst);
});

/**
 * Every bit explains something DIFFERENT -- a content assertion, not a hover
 * one, so it runs on both projects and drives the tips the way each platform
 * does (tap on the phone, hover on the desk). It caught a real defect once: the
 * strip rendered seven bits that all opened the same tooltip.
 */
test('each status bit explains something different', async ({ page }) => {
  await openConsole(page);
  const bits = page.locator('#stats .bit');
  const seen = new Set<string>();
  const n = Math.min(await bits.count(), 4);
  for (let i = 0; i < n; i++) {
    await openTip(page, bits.nth(i));
    await expect(page.locator('#tip')).not.toHaveClass(/hide/);
    seen.add(((await page.locator('#tipt').textContent()) ?? '').trim());
  }
  expect(seen.size, 'status bits share a tooltip title').toBe(n);
});

/**
 * The gas rows during warm-up. The mock's default history has the SGP41
 * appear a third of the way in and its indices go live at two thirds, so the
 * VOC row must draw SOMETHING (the raw ticks) rather than a flat zero, and
 * the readout must say which regime the newest sample is in.
 */
test('the VOC row shows data and a labelled readout', async ({ page }) => {
  await openConsole(page);
  await page.locator('#chips button[data-key="voc"]').click();
  await expect(page.locator('#sub_v')).not.toHaveClass(/hide/);
  // The newest mock samples carry live indices, so the readout names one.
  await expect(page.locator('#roV')).toContainText(/index \d+/, { timeout: 15_000 });
  await expect(page.locator('#roV')).toContainText(/raw \d+/);
});

test('the power row plots the plug watts', async ({ page }) => {
  await openConsole(page);
  await page.locator('#chips button[data-key="power"]').click();
  await expect(page.locator('#sub_w')).not.toHaveClass(/hide/);
  await expect(page.locator('#roW')).toContainText(/W$/, { timeout: 15_000 });
});

test('an outage is explained, not just left as a hole', async ({ page }) => {
  // The chart already breaks its lines across a gap. What this pins is the
  // half that was missing: the console asks WHY, and keeps drawing when the
  // answer is unavailable.
  const boots = recordRequests(page, /\/api\/boots/);
  await openConsole(page);
  await expect.poll(() => boots.length).toBeGreaterThan(0);
  await expect(boots[0]!.url()).toMatch(/days=\d+/);
});

test('a controller with no restart marks still draws its charts', async ({ page }) => {
  // /api/boots is a separate request precisely so it can fail alone: an older
  // firmware has no such route, and a missing explanation must never withhold
  // the data it would have explained.
  await page.route('**/api/boots*', (r) => r.fulfill({ status: 404, body: '{"error":"nope"}' }));
  await openConsole(page);
  await expect(page.locator('#cv_t')).toBeVisible();
  await expect(page.locator('#tcap')).not.toContainText(/could not load/i);
});

/**
 * The chart must not move while a finger is reading it.
 *
 * Four separate causes so far, all the same shape -- something above the plots
 * re-measures when the scrub content replaces the live content:
 *
 *  1. the sticky readout grew when it gained a weekday at 7D (+32 px);
 *  2. the state sentence wraps to a different line count when it describes a
 *     past moment (-17 px at 412, +19 px at 320);
 *  3. the hero badge went from `NOW` to `19:33 · 13H20 AGO`, which widened the
 *     hero's first column until the third column wrapped to a second row
 *     (+79 px) -- CI caught this one and local runs could not, because the
 *     webfonts do not load here and the fallback metrics are narrower;
 *  4. the same badge at 7D, 25 characters wide.
 *
 * These assertions are deliberately font-INDEPENDENT: each one says "this box
 * is the same size before and after", which is true in any font, rather than
 * naming pixel values that only hold in one. Both phone widths, because 320 and
 * 412 wrap the hero differently and (2) only showed up at 320.
 */
for (const width of [320, 412] as const) {
  for (const days of ['1', '7', '30'] as const) {
    test(`starting a scrub moves nothing at ${width}px on the ${days}-day range`, async ({ page }) => {
      await page.setViewportSize({ width, height: width === 320 ? 568 : 839 });
      await openConsole(page);
      if (days !== '1') await page.locator(`#ranges button[data-d="${days}"]`).click();
      await page.locator('#cv_t').scrollIntoViewIfNeeded();

      const shape = () =>
        page.evaluate(() => {
          const box = (sel: string) => {
            const el = document.querySelector(sel);
            if (!el) return null;
            const r = el.getBoundingClientRect();
            return { top: +(r.top + scrollY).toFixed(1), h: +r.height.toFixed(1), w: +r.width.toFixed(1) };
          };
          const hero = document.getElementById('hero')!;
          return {
            plotTop: box('#cv_t')!.top,
            hero: box('#hero'),
            // Column tops, so a rewrap is caught even if the height did not
            // happen to change with it.
            heroCols: [...hero.children].map((c) => +(c.getBoundingClientRect().top + scrollY).toFixed(1)),
            reason: box('#reason'),
            chead: box('#chead'),
            stampW: box('#stamp')!.w,
          };
        });

      const before = await shape();
      const plot = await page.locator('#cv_t').boundingBox();
      if (!plot) return;
      await page.mouse.move(plot.x + plot.width * 0.5, plot.y + plot.height * 0.5);
      await expect(page.locator('#chtitle')).toHaveText(/\d{1,2}:\d{2}/);
      const after = await shape();

      // The badge's fixed footprint is the MECHANISM behind the hero half: a
      // constant-width box cannot re-measure the column it sits in.
      expect(after.stampW, 'the hero badge changed width on scrub').toBe(before.stampW);
      expect(after.hero, 'the hero re-measured on scrub').toEqual(before.hero);
      expect(after.heroCols, 'a hero column moved on scrub').toEqual(before.heroCols);
      expect(after.reason, 'the state sentence re-measured on scrub').toEqual(before.reason);
      expect(after.chead, 'the sticky readout re-measured on scrub').toEqual(before.chead);
      expect(after.plotTop, 'the plot moved under the scrub').toBe(before.plotTop);
    });
  }
}

test('the row chips are thumb-sized and say the strip continues', async ({ page }) => {
  await page.setViewportSize({ width: 320, height: 568 });
  await openConsole(page);
  const chips = page.locator('#chips');
  // Seven chips need 332 px in a 288 px column, so at this width the strip
  // MUST advertise itself as scrollable -- a silent clip reads as a bug.
  await expect(chips).toHaveClass(/more/);
  for (const i of [0, 3, 6]) {
    const b = await page.locator('#chips button').nth(i).boundingBox();
    expect(b!.height, `chip ${i} is under 44 px tall`).toBeGreaterThanOrEqual(44);
  }
  // NOX is the last one and the one that used to be unreachable.
  await chips.evaluate((el) => el.scrollTo({ left: el.scrollWidth }));
  await expect(chips).not.toHaveClass(/more/);
  const inView = await page.evaluate(() => {
    const host = document.getElementById('chips')!;
    const nox = host.querySelector('[data-key="nox"]')!.getBoundingClientRect();
    const box = host.getBoundingClientRect();
    return nox.left >= box.left - 1 && nox.right <= box.right + 1;
  });
  expect(inView, 'NOX is still not on screen after scrolling to the end').toBeTruthy();
  await page.locator('#chips button[data-key="nox"]').click();
  await expect(page.locator('#sub_n')).not.toHaveClass(/hide/);
});

test('the range chips are thumb-sized and separated', async ({ page }) => {
  await page.setViewportSize({ width: 320, height: 568 });
  await openConsole(page);
  const boxes = await page.locator('#ranges button').evaluateAll((els) =>
    els.map((e) => { const r = e.getBoundingClientRect(); return { x: r.x, y: r.y, w: r.width, h: r.height }; }),
  );
  expect(boxes).toHaveLength(6);
  for (const [i, b] of boxes.entries()) {
    expect(b.w, `range chip ${i} is ${b.w} px wide`).toBeGreaterThanOrEqual(44);
    expect(b.h, `range chip ${i} is ${b.h} px tall`).toBeGreaterThanOrEqual(44);
  }
  // Adjacent chips must not share an edge, or one thumb hits two ranges.
  // Within a row only: the strip wraps at six ranges, and the first chip of a
  // new line legitimately sits left of the last one above it.
  for (let i = 1; i < boxes.length; i++) {
    if (boxes[i]!.y !== boxes[i - 1]!.y) continue;
    expect(boxes[i]!.x - (boxes[i - 1]!.x + boxes[i - 1]!.w)).toBeGreaterThanOrEqual(4);
  }
});
