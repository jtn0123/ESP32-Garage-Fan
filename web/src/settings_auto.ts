// The settings that tune auto mode: the differential band with the gas boost
// layered under it, and the absolute temperature limits that bound both.
// Split from settings.ts, which had outgrown the 500-line ceiling.

import { FLOOR_RESUME_F } from './reason.js';
import type { Group, Row, SettingsDeps } from './settings.js';

export const clamp = (v: number, lo: number, hi: number): number =>
  Math.min(hi, Math.max(lo, Number(v.toFixed(2))));

export const step = (
  label: string,
  hint: string,
  value: string,
  dec: () => void,
  inc: () => void,
): Row => ({ kind: 'step', label, hint, value, dec, inc });

/**
 * How far apart the steppers keep the low limit and the start point, °F. The
 * firmware copes with any pair -- a start point under the low limit's resume
 * margin is simply inert -- but a drawer reading "start above 63, low limit
 * 64" would be describing a band that does not exist.
 */
const LIMIT_SPAN_F = 2;

const degrees = (f: number): string => `${Number(f.toFixed(1))} °F`;

export function autoGroups(d: SettingsDeps): Group[] {
  const s = d.state;
  const set = d.setConfig;
  const eng = s.on_f;
  const rel = s.off_f;

  return [
    {
      title: 'AUTO MODE',
      blurb:
        'The differential band that decides when the fan runs on its own. A wider band means fewer start/stop cycles. Engaging is instant; once running, auto commits for at least 15 minutes before it may drop back.',
      rows: [
        step(
          'Engage above',
          'Garage must be this many degrees hotter than the yard before auto drives the fan to its hold speed.',
          `+${eng.toFixed(1)} °F`,
          // Release must stay strictly below engage or the hysteresis latch
          // flaps; the firmware enforces this too, this just avoids the round trip.
          () => set(`onf=${clamp(eng - 0.5, rel + 0.5, 20)}`),
          () => set(`onf=${clamp(eng + 0.5, 0.5, 20)}`),
        ),
        step(
          'Release below',
          'Fan drops back to the rest speed once the gap falls under this. Keep it well under the engage point or the fan chatters.',
          `+${rel.toFixed(1)} °F`,
          () => set(`offf=${clamp(rel - 0.5, 0, 20)}`),
          () => set(`offf=${clamp(rel + 0.5, 0, eng - 0.5)}`),
        ),
        step(
          'Hold speed',
          'Speed auto holds while the differential is above the engage point — partial venting wastes the gap.',
          `${s.auto_max} / 12`,
          () => set(`max=${Math.max(1, s.auto_max - 1)}`),
          () => set(`max=${Math.min(12, s.auto_max + 1)}`),
        ),
        step(
          'Rest speed',
          'Speed auto falls back to once inside and outside have equalized. Zero means the fan stops.',
          s.auto_min === 0 ? 'off' : `${s.auto_min} / 12`,
          () => set(`min=${Math.max(0, s.auto_min - 1)}`),
          () => set(`min=${Math.min(12, s.auto_min + 1)}`),
        ),
        {
          kind: 'toggle',
          label: 'Auto mode',
          hint: 'When off, the fan holds whatever speed you set by hand until you turn auto back on.',
          on: s.auto,
          toggle: d.toggleAuto,
        },
        {
          kind: 'toggle',
          label: 'Gas boost',
          hint: 'Bad air overrides a resting thermostat: while the VOC index is above the trigger, auto mode holds at least the boost speed. Releases 100 index points below the trigger, and never before 15 minutes of boosting. Needs the SGP41 warmed up — the boost simply stays off without it.',
          on: s.gas_on,
          toggle: () => d.setConfig(`gason=${s.gas_on ? 0 : 1}`),
        },
        step(
          'Gas boost · trigger',
          'VOC index that engages the boost. 100 is this sensor’s own 24 h average, so 250 means "clearly worse than normal for this garage".',
          `${s.gas_voc}`,
          () => set(`gasvoc=${Math.max(100, s.gas_voc - 25)}`),
          () => set(`gasvoc=${Math.min(500, s.gas_voc + 25)}`),
        ),
        step(
          'Gas boost · speed',
          'The minimum speed auto holds while the boost is engaged. The thermostat can still run faster; it cannot run slower.',
          `${s.gas_spd} / 12`,
          () => set(`gasspd=${Math.max(1, s.gas_spd - 1)}`),
          () => set(`gasspd=${Math.min(12, s.gas_spd + 1)}`),
        ),
      ],
    },
    {
      title: 'TEMPERATURE LIMITS',
      blurb:
        'For winter. Auto vents whenever the garage is warmer than the yard, which in winter is nearly always, so on its own it would vent the garage down toward the outdoor temperature. These keep it inside a comfort band. Each has its own switch, and the temperatures are kept while switched off.',
      rows: [
        {
          kind: 'toggle',
          label: 'Low limit',
          hint: `Once the garage cools to this, auto drops the fan to the rest speed however much warmer than the yard it is — at once, without waiting out the 15-minute minimum run. It may start again once the garage is ${FLOOR_RESUME_F} °F above the limit. The gas boost still runs on bad air.`,
          on: s.floor_on,
          toggle: () => set(`flooron=${s.floor_on ? 0 : 1}`),
        },
        step(
          'Low limit · temperature',
          'The coldest the fan will vent the garage to.',
          degrees(s.floor_f),
          () => set(`floorf=${clamp(s.floor_f - 1, 32, 100)}`),
          () => set(`floorf=${clamp(s.floor_f + 1, 32, Math.min(100, s.start_f - LIMIT_SPAN_F))}`),
        ),
        {
          kind: 'toggle',
          label: 'Start above',
          hint: 'Auto leaves the garage alone until it warms to this. Once started, the fan keeps going below it, until the gap closes or the low limit stops it — so pair it with the low limit in winter, or one warm afternoon vents the garage all the way down to outdoor temperature.',
          on: s.start_on,
          toggle: () => set(`starton=${s.start_on ? 0 : 1}`),
        },
        step(
          'Start above · temperature',
          'The garage temperature at which auto starts venting.',
          degrees(s.start_f),
          () => set(`startf=${clamp(s.start_f - 1, Math.max(32, s.floor_f + LIMIT_SPAN_F), 120)}`),
          () => set(`startf=${clamp(s.start_f + 1, 32, 120)}`),
        ),
      ],
    },
  ];
}
