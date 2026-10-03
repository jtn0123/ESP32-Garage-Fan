// History mode: what the top of the console looks like while it describes a
// logged moment instead of the device right now.
//
// Scrubbing used to repaint the three hero numbers and the sentence, and leave
// everything around them live: the pills, the metric strip and the speed rail
// went on asserting "auto on", "Strong", "20.3 W" and a lit 9 beside a
// sentence saying the fan was OFF at 10:12. Half the hero was the past and
// half was now, and the only thing telling them apart was a five-character
// stamp. This module owns the difference: one class on #console (`past`, plus
// `held` once a moment is pinned), the "VIEWING 10:51 · 9H50 AGO" bar, and the
// moment's own speed, airflow, draw and duty where the log has them. past.css
// turns the hero into a tinted history panel around them. All of it is
// presentation: which moment is shown, and how it gets pinned or released, is
// scrub.ts's business.

import { $, at } from './dom.js';
import { ago, airflow, moment } from './format.js';
import { minutesBack } from './history_view.js';
import { paintRail } from './rail.js';
import { view } from './state.js';

/**
 * "10:51 · 9H50 AGO" -- the moment, at the precision its range needs, and how
 * long ago it was. The same words as the chart header readout, so the two
 * places that name the moment never disagree.
 */
export function pastLine(t: number | null, days: number, minutes: number): string {
  return t === null ? ago(minutes) : `${moment(t, days)} · ${ago(minutes)}`;
}

export interface PastMetrics {
  air: string;
  draw: string;
  duty: string;
}

/**
 * The metric strip as it read at a logged moment.
 *
 * Only what the log can back: speed and the plug's watts are columns in every
 * row since 1.14.23, and duty follows from the speed through the same capture
 * table the live tile uses. A row with no speed says '–' rather than "Still" --
 * an unlogged speed is not a stopped fan (see console.ts::loggedSpeed).
 */
export function pastMetrics(
  speed: number | undefined,
  watts: number | null,
  highUs: readonly number[] | undefined,
  periodUs: number,
): PastMetrics {
  const draw = watts === null ? '–' : `${watts.toFixed(1)} W`;
  if (speed === undefined) return { air: '–', draw, duty: '–' };
  const high = speed > 0 ? highUs?.[speed] : 0;
  const duty = high === undefined || periodUs <= 0 ? '–' : `${((high / periodUs) * 100).toFixed(1)}%`;
  return { air: airflow(speed), draw, duty };
}

/**
 * Put the console into (or out of) history mode for sample `i`, -1 for now.
 *
 * Called at the end of every paintHero, so it runs on each scrub sample and on
 * each SSE frame. Leaving history mode only drops the classes: restoring the
 * live rail and metric values is a full paint(), which scrub.ts runs on the way
 * out -- this module never has to know how the live values are derived.
 */
export function paintPast(i: number): void {
  const con = $('console');
  const s = view.series;
  const past = i >= 0 && s !== null;
  con.classList.toggle('past', past);
  con.classList.toggle('held', past && view.pinTs !== null);
  if (!past) return;

  $('pastwhen').textContent = pastLine(s.ts(i), view.days, minutesBack(s, i));

  // The moment's own values, in the live tiles' places.
  const speed = s.spd.length ? s.spd[i] : undefined;
  paintRail(speed ?? null, true);
  const m = pastMetrics(speed, at(s.w, i), view.info?.high_us, view.info?.period_us ?? 9934);
  $('mAir').textContent = m.air;
  $('mW').textContent = m.draw;
  $('pwmval').textContent = m.duty;
}
