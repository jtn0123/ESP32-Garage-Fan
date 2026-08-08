// Firmware version parsing and comparison.
//
// Ported from VoltTracker's UpdateFeed.kt, which learned these rules the hard
// way. Two of them matter more than the arithmetic:
//
//  1. UNKNOWN is a real answer. If either side fails to parse we say so rather
//     than folding it into "up to date" -- that would be a confident falsehood,
//     and the failure mode is a device that never offers a real update.
//  2. Out-of-range components produce null instead of a wrong number. The
//     packing formula collides once a component exceeds its decade, so a
//     version we cannot represent must not silently compare as something else.

/** Upper bounds implied by the packing below. Kept explicit so the guard reads. */
const MAX_MAJOR = 2_000;
const MAX_MINOR = 999;
const MAX_PATCH = 999;

const SEMVER = /^v?(\d+)\.(\d+)\.(\d+)$/;

/**
 * Collapse `major.minor.patch` into one monotonically increasing integer, or
 * null if the string is not a plain three-part version we can represent.
 *
 * A leading `v` is accepted so git tags (`v1.13.0`) and the firmware's own
 * FW_VERSION (`1.13.0`) both parse. Pre-release suffixes deliberately do NOT
 * parse: `v1.2.3-rc1` yields null, which surfaces as UNKNOWN rather than
 * offering a release candidate to a garage fan.
 */
export function versionCode(tag: string): number | null {
  const m = SEMVER.exec(tag.trim());
  if (!m) return null;
  const major = Number(m[1]);
  const minor = Number(m[2]);
  const patch = Number(m[3]);
  if (major > MAX_MAJOR || minor > MAX_MINOR || patch > MAX_PATCH) return null;
  return major * 1_000_000 + minor * 1_000 + patch;
}

export type Comparison = 'newer' | 'current' | 'older' | 'unknown';

/** Compare an available version against the running one. */
export function compare(running: number | null, available: number | null): Comparison {
  if (running === null || available === null) return 'unknown';
  if (available > running) return 'newer';
  if (available === running) return 'current';
  return 'older';
}

/**
 * Whether `availableTag` is an upgrade over `runningVersion`.
 *
 * `older` is treated exactly like `current`: we never offer a downgrade, so a
 * release that was yanked and re-tagged downward cannot walk a field device
 * backwards.
 */
export function isUpgrade(runningVersion: string, availableTag: string): Comparison {
  return compare(versionCode(runningVersion), versionCode(availableTag));
}
