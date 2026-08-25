// The restart-mark grouping and labels (charts.ts).
//
// Four OTA deploys on 2026-08-23 landed 38-80 px apart on the 6 h view under
// ~97 px labels, and the chart rendered "restar▶restar▶restart sw_reset" --
// four labels scribbled over each other at exactly the moment they mattered.
// The canvas cannot be asserted on in jsdom, but the grouping maths and the
// label wording are pure and exported for precisely this file.
import { describe, expect, it } from 'vitest';

import { bootLabel, clusterMarks } from '../src/charts.js';
import type { BootMark } from '../src/types.js';

const mark = (ts: number, cause = 'sw_reset', fw?: string): BootMark =>
  fw === undefined ? { ts, n: 1, cause } : { ts, n: 1, cause, fw };

describe('clusterMarks', () => {
  it('leaves well-separated marks alone', () => {
    expect(clusterMarks([10, 200, 500], 97)).toEqual([[0], [1], [2]]);
  });

  it('groups marks that sit under one label', () => {
    // The real 2026-08-23 spacing on the 6 h view: 38/49/80 px gaps.
    expect(clusterMarks([500, 538, 587, 667], 97)).toEqual([[0, 1, 2], [3]]);
  });

  it('measures from the group START, not the previous mark', () => {
    // A chain of 60 px gaps must not merge forever: each mark is within 97 px
    // of its neighbour, but a label anchored at the first stem has ENDED by
    // the third. Chaining pairwise would fuse the whole deploy day into one
    // group and hide that there were distinct sessions.
    expect(clusterMarks([0, 60, 120, 180], 97)).toEqual([[0, 1], [2, 3]]);
  });

  it('handles the empty and single cases', () => {
    expect(clusterMarks([], 97)).toEqual([]);
    expect(clusterMarks([42], 97)).toEqual([[0]]);
  });
});

describe('bootLabel', () => {
  it('keeps the old wording for a lone versionless mark', () => {
    expect(bootLabel([mark(1, 'brownout')])).toBe('restart · brownout');
    expect(bootLabel([mark(1, 'unknown')])).toBe('restart');
  });

  it('headlines the version when the record carries one', () => {
    // "sw_reset" names an OTA and a plain restart indistinguishably; the
    // version is the answer to the question the mark exists for.
    expect(bootLabel([mark(1, 'sw_reset', '1.26.0')])).toBe('restart · sw_reset → 1.26.0');
  });

  it('collapses a cluster to a count and the version that came out of it', () => {
    const g = [mark(1, 'sw_reset', '1.24.0'), mark(2, 'sw_reset', '1.25.0'), mark(3, 'sw_reset', '1.26.0')];
    expect(bootLabel(g)).toBe('3 restarts → 1.26.0');
  });

  it('uses the LAST version present, skipping trailing pre-1.26.0 rows', () => {
    const g = [mark(1, 'sw_reset', '1.26.0'), mark(2, 'sw_reset')];
    expect(bootLabel(g)).toBe('2 restarts → 1.26.0');
  });

  it('a versionless cluster is still a count, never an invented version', () => {
    expect(bootLabel([mark(1), mark(2)])).toBe('2 restarts');
  });
});
