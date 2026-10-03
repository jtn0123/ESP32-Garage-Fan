// revealBy: the scroll that brings the opened scope on screen (app.ts).
//
// On a phone the scope opens below the speed rail. Too little scroll and the
// tap looks dead; too much and the rail -- the reason the page is open -- goes
// off the top for no reason. These pin "the least movement that shows it".

import { describe, expect, it } from 'vitest';
import { revealBy } from '../src/dom.js';

describe('revealBy', () => {
  it('does not move a span that is already on screen', () => {
    // A Pixel 7: the scope opens at ~490 and its trace ends at ~720 of 839.
    expect(revealBy(490, 720, 839)).toBe(0);
  });

  it('scrolls down only as far as the foot, plus the pad', () => {
    // 320x568: the scope opens just under the fold.
    expect(revealBy(568, 820, 568)).toBe(820 - (568 - 12));
  });

  it('scrolls up to the head of a span above the screen', () => {
    expect(revealBy(-40, 200, 800)).toBe(-52);
  });

  it('leads with the head of a span taller than the screen', () => {
    // The title and the close button live at the top of the panel.
    expect(revealBy(300, 1300, 800)).toBe(288);
    expect(revealBy(-100, 900, 800)).toBe(-112);
  });

  it('treats the pad as part of what has to fit', () => {
    expect(revealBy(12, 788, 800)).toBe(0);
    expect(revealBy(12, 789, 800)).toBe(0); // too tall with both pads: head already placed
    expect(revealBy(20, 790, 800)).toBe(2);
  });
});
