// The live sentence under the gauge: why the fan is doing what it is doing
// right now. Pure -- state in, sentence out -- so the cases the hardware
// reaches only in winter (the low limit, the start gate) are pinned by unit
// tests instead of waited for.

import type { DeviceState } from './types.js';

/**
 * fan/auto_logic.h::kFloorResumeC, in °F: how far above the low limit the
 * garage must climb before auto may engage again. Pinned to the firmware by
 * tests/test_web_contract.py.
 */
export const FLOOR_RESUME_F = 1.5;

/** "64°" / "64.5°": a limit as the sentences (and the gauge's held tag) name it. */
export const deg = (f: number): string => `${Number(f.toFixed(1))}°`;

/** "speed 6" / "off" -- a fan speed as the sentences name it. */
const speedWord = (n: number): string => (n > 0 ? `speed ${n}` : 'off');

/** "keeps the fan off" / "keeps the fan at speed 2" -- the rest speed as a phrase. */
const restPhrase = (s: DeviceState): string => (s.auto_min > 0 ? `at speed ${s.auto_min}` : 'off');

/** The low limit is holding the fan at rest. Needs only the garage reading. */
function floorReason(s: DeviceState, garage: number | null): string {
  const resume = deg(s.floor_f + FLOOR_RESUME_F);
  const gas = s.gas_active ? ' The gas boost is still running the fan for bad air.' : '';
  if (garage !== null && garage > s.floor_f) {
    return `Garage is ${garage.toFixed(1)}°F, just above your ${deg(s.floor_f)} low limit — auto waits for ${resume} before venting again, so the fan does not chatter at the limit.${gas}`;
  }
  const now = garage === null ? '' : ` at ${garage.toFixed(1)}°F`;
  return `Garage is down to your ${deg(s.floor_f)} low limit${now} — auto keeps the fan ${restPhrase(s)} rather than vent it any colder, however warm it is next to the yard. It may start again above ${resume}.${gas}`;
}

/** No limit in charge: the differential against the engage/release band. */
function gapReason(s: DeviceState, delta: number): string {
  const gap = delta.toFixed(1);
  const rest = speedWord(s.auto_min);
  if (delta >= s.on_f) {
    const floor = s.floor_on
      ? ` It stops at once if the garage cools to your ${deg(s.floor_f)} low limit.`
      : '';
    return `Garage is ${gap}°F hotter than the yard — past the +${s.on_f}° engage point, so auto is holding speed ${s.auto_max}. It falls back to ${rest} when the gap drops under +${s.off_f}°, though never before 15 minutes of running.${floor}`;
  }
  if (delta <= s.off_f) {
    return `Garage is only ${gap}°F hotter than the yard — under the +${s.off_f}° release point, so auto has dropped the fan to ${rest}. It engages speed ${s.auto_max} again above +${s.on_f}°.`;
  }
  return `Gap is ${gap}°F, inside the +${s.off_f}°/+${s.on_f}° deadband — auto is holding ${speedWord(s.speed)} until it crosses a threshold, so the fan does not chatter.`;
}

/**
 * `garage` is the reading the hero shows, °F, or null when there is none. The
 * firmware's `limit` verdict outranks the thresholds: it is the only thing
 * that knows whether a limit, rather than the differential, is in charge.
 */
export function liveReason(s: DeviceState, garage: number | null): string {
  // Before the no-outdoor-reading case: the low limit needs only the garage.
  if (s.auto && s.limit === 'floor') return floorReason(s, garage);
  if (garage === null || s.outside_f === null) {
    return 'No outdoor reading yet — auto holds the last speed rather than guessing. The fan fetches the outside temperature from open-meteo every 10 minutes.';
  }
  if (!s.auto) {
    return `Auto is off — the fan is at ${speedWord(s.speed)} because you set it by hand. Turn auto back on to let the differential drive it again.`;
  }
  const delta = garage - s.outside_f;
  if (s.limit === 'start') {
    return `Garage is ${delta.toFixed(1)}°F hotter than the yard, but at ${garage.toFixed(1)}°F it is under your ${deg(s.start_f)} start point — auto leaves the fan ${restPhrase(s)} until the garage warms past it.`;
  }
  return gapReason(s, delta);
}
