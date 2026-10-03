// The fan-state headline: what the fan is doing, in one word, before anyone
// has read the sentence under the gauge. Pure -- state in, headline out -- so
// the winter limits and the gas boost are pinned by unit tests rather than
// waited for, the same contract reason.ts keeps for the sentence itself.
//
// The word answers WHAT; reason.ts still answers WHY. They must never
// disagree, which is why both read the same fields in the same priority:
// the firmware's own `limit` verdict outranks anything inferred from speeds.

import type { DeviceState } from './types.js';

/**
 * Which colour family the headline wears (console.css maps each to a theme
 * colour). `run` the accent blue of the rail; `rest` muted; `floor` and
 * `start` a winter limit is in charge, in that limit's own colour (theme.ts
 * LIMIT_COLOURS) so the word, the held gauge and the chart line all agree;
 * `hold` a missing reading is; `hand` you are; `gas` the VOC row's colour;
 * `fault` the controller cannot say what the fan is doing.
 */
export type HeadTone = 'run' | 'rest' | 'floor' | 'start' | 'hold' | 'hand' | 'gas' | 'fault';

export interface Headline {
  /** The state, upper case: VENTING, RESTING, HELD, ... */
  word: string;
  /** What qualifies it ("LOW LIMIT"), or null when the word stands alone. */
  detail: string | null;
  /** "speed 9" / "fan off" -- null when the word already says it (OFF). */
  speed: string | null;
  tone: HeadTone;
  /** Seconds per turn of the icon; null holds it still. */
  spin: number | null;
}

/**
 * Seconds per turn for a speed: 2.7 s at 1, 1.2 s at 6, 0.9 s at 9, 0.72 s
 * at 12. Hyperbolic rather than linear so the low steps -- where the real fan
 * is barely moving -- stay visibly slow, and 12 is brisk without strobing a
 * three-blade glyph. Null at rest and for a raw or unknown output, which has
 * no step to scale by.
 */
export function spinSeconds(speed: number): number | null {
  if (speed <= 0) return null;
  return Math.round((3.6 / (1 + speed / 3)) * 100) / 100;
}

/** "speed 9" / "fan off" -- a bare "off" after RESTING read as a typo. */
const speedText = (n: number): string => (n > 0 ? `speed ${n}` : 'fan off');

function make(word: string, detail: string | null, tone: HeadTone, s: DeviceState): Headline {
  return { word, detail, speed: speedText(s.speed), tone, spin: spinSeconds(s.speed) };
}

/** Auto is on: which of its states is in charge right now. */
function autoHeadline(s: DeviceState): Headline {
  // The gas floor only ever RAISES the thermostat's speed (fan_apply_gas_floor),
  // so it is the reason only while the fan sits at or under the boost speed.
  // Above it the thermostat is venting on its own and the boost is moot.
  if (s.gas_active && s.speed > 0 && s.speed <= s.gas_spd) {
    return make('GAS BOOST', null, 'gas', s);
  }
  if (s.limit === 'floor') return make('HELD', 'LOW LIMIT', 'floor', s);
  if (s.limit === 'start') return make('WAITING', 'START POINT', 'start', s);
  // No yard reading: auto holds the last speed rather than guessing, so
  // neither VENTING nor RESTING would be the thermostat's verdict.
  if (s.outside_f === null) return make('HOLDING', 'NO OUTDOOR READING', 'hold', s);
  return s.speed > s.auto_min ? make('VENTING', null, 'run', s) : make('RESTING', null, 'rest', s);
}

export function fanHeadline(s: DeviceState): Headline {
  if (s.actuator_fault) {
    return { word: 'FAULT', detail: 'PWM REJECTED', speed: null, tone: 'fault', spin: null };
  }
  // -1 is a raw duty sent over MQTT, -2 an output nobody can vouch for after
  // a boot driver failure. Neither has a step to name or to spin at.
  if (s.speed === -1) {
    return { word: 'RAW DUTY', detail: null, speed: null, tone: 'hand', spin: null };
  }
  if (s.speed < 0) {
    return { word: 'UNKNOWN', detail: 'OUTPUT', speed: null, tone: 'fault', spin: null };
  }
  if (!s.auto) {
    if (s.speed === 0) return { word: 'OFF', detail: null, speed: null, tone: 'rest', spin: null };
    return make('MANUAL', null, 'hand', s);
  }
  return autoHeadline(s);
}

/** The whole headline as one phrase, for a tooltip and assistive tech. */
export function headlineLabel(h: Headline): string {
  return [h.word, h.detail, h.speed].filter((p) => p !== null).join(' · ');
}
