import { describe, expect, it } from 'vitest';
import { compare, isUpgrade, versionCode } from '../src/version.js';

describe('versionCode', () => {
  it('parses with and without the tag prefix', () => {
    expect(versionCode('1.13.0')).toBe(1_013_000);
    expect(versionCode('v1.13.0')).toBe(1_013_000);
    expect(versionCode('  v1.13.0  ')).toBe(1_013_000);
  });

  it('orders monotonically across component boundaries', () => {
    const asc = ['0.0.1', '0.1.0', '0.9.9', '1.0.0', '1.2.3', '1.13.0', '2.0.0'];
    const codes = asc.map((v) => versionCode(v));
    expect(codes.every((c) => c !== null)).toBe(true);
    for (let i = 1; i < codes.length; i++) {
      expect(codes[i]!).toBeGreaterThan(codes[i - 1]!);
    }
  });

  it('does not let a large minor overtake a major bump', () => {
    // The whole point of the decade bounds: 1.999.0 must stay below 2.0.0.
    expect(versionCode('1.999.0')!).toBeLessThan(versionCode('2.0.0')!);
  });

  it('rejects anything it cannot represent rather than guessing', () => {
    expect(versionCode('1.1000.0')).toBeNull(); // would collide with 2.0.0
    expect(versionCode('1.0.1000')).toBeNull();
    expect(versionCode('2001.0.0')).toBeNull();
  });

  it('rejects pre-releases and malformed tags', () => {
    for (const bad of ['v1.2.3-rc1', '1.2', '1.2.3.4', 'latest', '', 'v', 'dev', '1.2.x']) {
      expect(versionCode(bad)).toBeNull();
    }
  });
});

describe('compare', () => {
  it('classifies the three ordered cases', () => {
    expect(compare(1_000_000, 1_013_000)).toBe('newer');
    expect(compare(1_013_000, 1_013_000)).toBe('current');
    expect(compare(1_013_000, 1_000_000)).toBe('older');
  });

  it('reports unknown when either side failed to parse', () => {
    expect(compare(null, 1_013_000)).toBe('unknown');
    expect(compare(1_013_000, null)).toBe('unknown');
    expect(compare(null, null)).toBe('unknown');
  });
});

describe('isUpgrade', () => {
  it('treats an untagged dev build as unknown, never as up to date', () => {
    // A working tree builds FW_VERSION from `git describe`, e.g. "v1.13.0-2-gabc1234".
    // Claiming "up to date" there would be a confident falsehood.
    expect(isUpgrade('v1.13.0-2-gabc1234', 'v1.14.0')).toBe('unknown');
    expect(isUpgrade('dev', 'v1.14.0')).toBe('unknown');
  });

  it('never offers a downgrade', () => {
    expect(isUpgrade('1.13.0', 'v1.12.0')).toBe('older');
  });
});
