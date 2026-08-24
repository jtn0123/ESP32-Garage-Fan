// Painters for the live half of the console screen: hero numbers, gauge, PWM
// readouts, the scope, odometer tiles and status bits. Everything reads the
// shared view-model in state.ts and writes only to the DOM.
//
// The recorded half -- the charts, their captions and readouts -- lives in
// history_view.ts, which this file leans on only for the scrub stamp.
//
// The one thing this module does NOT know is the settings screen -- that
// needs the command functions in app.ts, which imports us. app.ts registers
// its settings painter through setSettingsPainter to keep the dependency one
// way.

import { SERIES_COLOURS } from './charts.js';
import { $, at, el, show } from './dom.js';
import { ago, airflow, clock, hoursMinutes, moment, signed } from './format.js';
import { paintChartTitle } from './history_view.js';
import { paintRail } from './rail.js';
import { paintBits } from './status_bits.js';
import { drawScope, msPerDivision, type Waveform } from './pwm.js';
import { ROW_IDS, sampleIndex, view, type RowKey } from './state.js';
import { AC, FAI, OK, OR, OUT, PU, TX } from './theme.js';
import type { DeviceState } from './types.js';

let settingsPainter: (() => void) | null = null;
export function setSettingsPainter(fn: () => void): void {
  settingsPainter = fn;
}

/* ------------------------------------------------------------------ waveform */

export function waveform(): Waveform {
  const period = view.info?.period_us ?? 9934;
  const speed = view.state?.speed ?? 0;
  const highUs = speed > 0 ? (view.info?.high_us[speed] ?? 0) : 0;
  return { duty: highUs / period, highUs, periodUs: period };
}

function scopeTick(): void {
  if (!view.scopeOpen || view.screen !== 'console') {
    view.raf = null;
    return;
  }
  view.phase = (performance.now() / 1900) % 1;
  drawScope($<HTMLCanvasElement>('cv_pw'), waveform(), view.phase, performance.now());
  view.raf = requestAnimationFrame(scopeTick);
}

export function startScope(): void {
  drawScope($<HTMLCanvasElement>('cv_pw'), waveform(), view.phase, performance.now());
  if (view.raf === null) view.raf = requestAnimationFrame(scopeTick);
}

export function stopScope(): void {
  if (view.raf !== null) {
    cancelAnimationFrame(view.raf);
    view.raf = null;
  }
}

/* ------------------------------------------------------------------- painting */

export function paintPwm(): void {
  const w = waveform();
  const speed = view.state?.speed ?? 0;
  const pct = ((w.highUs / w.periodUs) * 100).toFixed(1);

  const value = $('pwmval');
  value.textContent = `${pct}%`;
  value.className = view.scopeOpen || view.previewOpen ? 'lit' : '';
  const hint = $('pwmhint');
  hint.textContent = view.scopeOpen ? 'LIVE ▾' : 'SIGNAL';
  hint.className = view.scopeOpen ? 'lit' : '';
  $('pwmhigh').textContent = speed > 0 ? `${w.highUs} µs high` : 'line held low';
  $('pwmhz').textContent = `${(1e6 / w.periodUs).toFixed(1)} Hz`;
  if (!view.scopeOpen) return;

  const live = speed > 0;
  const dot = $('scdot');
  dot.style.background = live ? OK : '#3d4653';
  dot.style.boxShadow = `0 0 9px ${live ? OK : '#3d4653'}`;
  const badge = $('scstate');
  badge.textContent = live ? 'TRANSMITTING' : 'IDLE';
  badge.style.background = live ? 'rgba(34,160,107,.16)' : '#111823';
  badge.style.color = live ? OK : '#4a5a6e';
  $('scdiv').textContent = `${msPerDivision(w.periodUs)} ms/div`;

  const steps = (view.info?.high_us.length ?? 13) - 1;
  const cells: [string, string, string][] = [
    ['HIGH', `${w.highUs} µs`, AC],
    ['LOW', `${w.periodUs - w.highUs} µs`, OUT],
    ['FRAME', `${w.periodUs} µs`, OUT],
    ['FREQUENCY', `${(1e6 / w.periodUs).toFixed(1)} Hz`, OUT],
    ['DUTY', `${pct}%`, TX],
    ['STEPS LEARNED', `${steps} / ${steps}`, OK],
  ];
  const host = $('scstats');
  host.replaceChildren(
    ...cells.map(([k, v, colour]) => {
      const cell = el('div');
      cell.style.borderTopColor = colour;
      cell.append(el('div', { className: 'sk', textContent: k }));
      const val = el('div', { className: 'sv', textContent: v });
      val.style.color = colour;
      cell.append(val);
      return cell;
    }),
  );

  const grid = $('capgrid');
  Array.from(grid.children).forEach((b, n) => {
    b.className = n === speed ? 'on' : '';
  });
}

export function paintHero(): void {
  const s = view.state;
  if (!s) return;
  const series = view.series;
  const i = sampleIndex();
  const scrubbing = view.scrub >= 0 && i >= 0 && series !== null;

  let garage: number | null = view.live?.ok ? view.live.temp_c * 9 / 5 + 32 : null;
  let outside: number | null = s.outside_f;
  if (scrubbing) {
    garage = at(series.tf, i);
    outside = at(series.of, i);
  } else if (garage === null && series && series.n) {
    garage = at(series.tf, series.n - 1);
  }

  $('tT').textContent = garage === null ? '–' : garage.toFixed(1);
  $('tO').textContent = outside === null ? '–' : outside.toFixed(1);
  const delta = garage !== null && outside !== null ? garage - outside : null;
  $('tD').textContent = delta === null ? '–' : signed(delta);

  // Gauge geometry: 0 °F sits at 46%, each degree is 6.4% of the track.
  const pos = (v: number): number => Math.max(2, Math.min(97, 46 + v * 6.4));
  const band = $('gband');
  band.style.left = `${pos(s.off_f)}%`;
  band.style.width = `${Math.max(0, pos(s.on_f) - pos(s.off_f))}%`;
  $('gtr').style.left = `${pos(s.off_f)}%`;
  $('gte').style.left = `${pos(s.on_f)}%`;
  const relLabel = $('glr');
  relLabel.style.left = `${pos(s.off_f)}%`;
  relLabel.textContent = `RELEASE +${s.off_f} ◂`;
  const engLabel = $('gle');
  engLabel.style.left = `${pos(s.on_f)}%`;
  engLabel.textContent = `▸ ENGAGE +${s.on_f}`;
  const marker = $('gmark');
  marker.style.left = `${delta === null ? 46 : pos(delta)}%`;
  marker.style.opacity = delta === null ? '0.25' : '1';

  const stamp = $('stamp');
  if (scrubbing) {
    const t = series.ts(i);
    // The CLOCK only -- no date, no age. This badge shares a flex row with the
    // GARAGE label inside the hero's first column, so its width feeds straight
    // into that column's width: at `19:33 · 13H20 AGO` (and worse, 25 characters
    // at 7D) it outgrew the 92 px temperature beside it, the third hero column
    // wrapped to a second row, and everything below -- pills, metric strip,
    // rail, the whole chart stack -- slid 79 px DOWN mid-gesture. The full
    // moment, weekday and age all live in the chart header readout, which is
    // pinned above the plots and is the copy a thumb can actually see.
    stamp.textContent = t === null ? '–' : clock(t);
    stamp.className = 'scrub';
  } else if (view.pollFail > 0) {
    // A poll has failed but the verdict has not landed yet. The reading on
    // screen is as old as the last answer, so the badge says so instead of
    // asserting NOW for the next 20 seconds.
    const age = view.lastOk ? (Date.now() - view.lastOk) / 60_000 : Number.NaN;
    stamp.textContent = view.lastOk ? ago(age) : 'NO CONTACT';
    stamp.className = 'stale';
  } else {
    stamp.textContent = 'NOW';
    stamp.className = '';
  }
  paintChartTitle();

  $('reason').textContent = reason(delta, scrubbing, i);
}

/**
 * What the fan was doing at a logged moment: "running at 7", "off", or the
 * honest third answer.
 *
 * "not logged" is not "off". Rows written before the speed column existed have
 * no speed in them at all, and a sentence that says the fan was off at a moment
 * nobody recorded is inventing history.
 */
function loggedSpeed(speed: number | undefined): string {
  if (speed === undefined) return 'not logged';
  return speed > 0 ? `running at ${speed}` : 'off';
}

function reason(delta: number | null, scrubbing: boolean, i: number): string {
  const s = view.state;
  if (!s) return '';
  const rest = s.auto_min > 0 ? `speed ${s.auto_min}` : 'off';

  if (scrubbing && view.series) {
    const t = view.series.ts(i);
    // Same precision rule as the stamp: on a 60-day chart "At 15:27" names
    // sixty different moments and answers nothing.
    const when = t === null ? 'that sample' : moment(t, view.days);
    const fanWas = loggedSpeed(view.series.spd.length ? view.series.spd[i] : undefined);
    const gap = delta === null ? '–' : delta.toFixed(1);
    // No "move off the chart to return to now" tail any more. It was 36
    // characters of instruction for a gesture the reader has already performed,
    // and this sentence has to fit inside the box reserved for the live one it
    // replaces (scrub.ts::freezeReason) -- the live sentences are never shorter
    // than three lines at 320 px, and this is at most three.
    return `At ${when} the garage was ${gap}°F hotter than the yard and the fan was ${fanWas}.`;
  }
  if (delta === null) {
    return 'No outdoor reading yet — auto holds the last speed rather than guessing. The fan fetches the outside temperature from open-meteo every 10 minutes.';
  }
  if (!s.auto) {
    return `Auto is off — the fan is at ${s.speed > 0 ? `speed ${s.speed}` : 'off'} because you set it by hand. Turn auto back on to let the differential drive it again.`;
  }
  const gap = delta.toFixed(1);
  if (delta >= s.on_f) {
    return `Garage is ${gap}°F hotter than the yard — past the +${s.on_f}° engage point, so auto is holding speed ${s.auto_max}. It falls back to ${rest} when the gap drops under +${s.off_f}°, though never before 15 minutes of running.`;
  }
  if (delta <= s.off_f) {
    return `Garage is only ${gap}°F hotter than the yard — under the +${s.off_f}° release point, so auto has dropped the fan to ${rest}. It engages speed ${s.auto_max} again above +${s.on_f}°.`;
  }
  return `Gap is ${gap}°F, inside the +${s.off_f}°/+${s.on_f}° deadband — auto is holding ${s.speed > 0 ? `speed ${s.speed}` : 'off'} until it crosses a threshold, so the fan does not chatter.`;
}

export function paintStats(): void {
  const st = view.stats;
  if (!st) return;
  // The estimate only fills in when there is no watt meter reading.
  if (!view.state?.plug) $('mW').textContent = `${st.watts_now.toFixed(0)} W`;
  $('mRun').textContent = hoursMinutes(st.run_today_s);
  const tiles: [string, string, string, string][] = [
    ['FAN TODAY', (st.run_today_s / 3600).toFixed(1), 'h', AC],
    ['LIFETIME', (st.run_total_s / 3600).toFixed(0), 'h', OUT],
    ['ENERGY EST', (st.energy_wh / 1000).toFixed(2), 'kWh', PU],
    ['24H RANGE', st.samples ? `${st.t_min_f.toFixed(0)}–${st.t_max_f.toFixed(0)}` : '–', '°F', OR],
    ['24H AVERAGE', st.samples ? st.t_avg_f.toFixed(1) : '–', '°F', OR],
  ];
  $('odo').replaceChildren(
    ...tiles.map(([label, value, unit, colour]) => {
      const cell = el('div');
      cell.append(el('div', { className: 'k9', textContent: label }));
      const row = el('div', { className: 'ov' });
      const b = el('b', { textContent: value });
      b.style.color = colour;
      row.append(b, el('i', { textContent: unit }));
      cell.append(row);
      return cell;
    }),
  );
}

export function paintChips(): void {
  Array.from($('chips').children).forEach((child) => {
    const b = child as HTMLElement;
    const key = b.dataset['key'] as RowKey;
    const on = view.rows[key];
    b.style.color = on ? TX : FAI;
    b.style.borderColor = on ? SERIES_COLOURS[key] : 'transparent';
    show($(ROW_IDS[key].sub), on);
  });
}

/**
 * Is `next` actually newer than what we are already showing?
 *
 * Three sources write state -- the 15 s poll, the SSE stream, and every
 * command's own response -- and none of them was ordered against the others.
 * A poll issued before a speed change could land after it and repaint the rail
 * back to the old speed for a full cycle. uptime_s is monotonic within a boot
 * and `boots` increments across one, so the pair orders every frame without a
 * wire change.
 */
export function isNewer(next: DeviceState, cur: DeviceState | null): boolean {
  if (!cur) return true;
  if (next.boots !== cur.boots) return next.boots > cur.boots;
  return next.uptime_s >= cur.uptime_s;
}

/**
 * What the page says when it has never reached the controller at all.
 *
 * The cold-load case: you walk into the garage, the fan is not running, you
 * open the console -- and it used to sit on "Connecting to the controller…"
 * indefinitely, because the whole offline path was gated on having had one
 * successful poll to go stale from. A first-fetch failure is as much
 * information as a later one.
 */
const NO_CONTACT_REASON =
  'No answer from the controller. The page keeps retrying every 15 seconds — ' +
  'if this persists, check that the board has power and is on the network ' +
  '(garage-fan.local). Nothing below is a live reading.';



/**
 * The broker chip: one decision, so one function returning both halves.
 *
 * Four states in priority order. "We cannot reach the DEVICE" outranks anything
 * the device would have told us about the broker -- that flag is a last-known
 * value like every other, and a green "BROKER UP" on a controller that has not
 * answered in minutes is the single most misleading thing this page can say.
 * Then: nothing heard yet (boot), and the device's own view.
 */
function brokerChip(stale: boolean, s: DeviceState | null): { text: string; cls: string } {
  if (stale) return { text: '● NO CONTACT', cls: 'stale' };
  if (!s) return { text: '● BROKER —', cls: '' };
  if (s.mqtt) return { text: '● BROKER UP', cls: 'up' };
  return { text: '● BROKER DOWN', cls: '' };
}

export function paint(next?: DeviceState): void {
  if (next) {
    view.lastOk = Date.now();
    view.offline = false;
    view.pollFail = 0;
    // A straggler still proves the device is reachable, so it clears `offline`
    // -- but it must not be allowed to skip the repaint, or the page keeps
    // showing "● NO CONTACT" and stays dimmed while the state says otherwise,
    // for up to a full poll cycle. Drop only the stale VALUES, not the render.
    if (isNewer(next, view.state)) view.state = next;
  }
  const s = view.state;
  const stale = view.offline;

  // The broker chip and the body tint are painted BEFORE the no-state guard:
  // "we cannot reach it" is precisely the case with no frame to read, and the
  // old early return meant a cold failure repainted nothing at all. Offline
  // also outranks every field below -- they are all last-known values, and the
  // broker flag in particular would otherwise keep asserting a green "UP"
  // about a device that has not answered in minutes.
  const mqtt = $('hmq');
  const chip = brokerChip(stale, s);
  mqtt.textContent = chip.text;
  mqtt.className = chip.cls;
  document.body.classList.toggle('offline', stale);
  if (!s) {
    if (stale) {
      $('reason').textContent = NO_CONTACT_REASON;
      // The badge cannot be left saying NOW over an en dash: there is no
      // reading, current or otherwise.
      const stamp = $('stamp');
      stamp.textContent = 'NO CONTACT';
      stamp.className = 'stale';
    }
    return;
  }

  const batt = $('hbat');
  if (s.batt) {
    show(batt, true);
    batt.textContent =
      (s.batt.chg ? '⚡ ' : '') +
      (s.batt.pct !== null ? `${s.batt.pct}%` : `${s.batt.v.toFixed(2)} V`);
  } else {
    show(batt, false);
  }
  $('hfw').textContent = `FW ${s.fw}`;

  paintRail(s.speed);

  $('bauto').className = `pill${s.auto ? ' on' : ''}`;
  $('bauto').textContent = s.auto ? 'auto on' : 'auto off';
  $('boff').className = `pill${s.speed === 0 ? ' on' : ''}`;
  $('mAir').textContent = airflow(s.speed);

  // DRAW prefers the watt meter over the cubic estimate whenever the
  // controller has a reading; paintStats respects the same preference.
  if (s.plug) {
    $('mW').textContent = `${s.plug.w.toFixed(1)} W`;
    $('mW').title = 'measured at the plug';
  }
  // The one alert this page can raise that nothing else can: the meter says
  // the fan is not doing what it was told (it ran at full power for a day
  // while everything here honestly said OFF, 2026-08-13).
  const warn = $('plugwarn');
  if (s.plug && s.plug.cycling && !stale) {
    // The more specific finding outranks the plain disagreement: the meter
    // saw the fan stop and restart repeatedly while ONE speed was held (the
    // 2026-08-20 night: nine hours at a held speed 10, ~4 W most of the time
    // with bursts at the speed-12 level). The controller's output did not
    // change; the fan is not honouring it.
    warn.textContent =
      `The fan is cycling on and off: the watt meter saw it stop and restart ` +
      `${s.plug.flips} times in the last 10 minutes while ` +
      `${s.speed > 0 ? `speed ${s.speed}` : 'off'} was held steady (reading ${s.plug.w.toFixed(1)} W now). ` +
      'The controller did not change its output — the fan is not following it. ' +
      'Set the fan OFF here, power-cycle it at the plug, then set the speed again; ' +
      'if it comes back, the control cable or the fan\'s own controller needs a look.';
    show(warn, true);
  } else if (s.plug && s.plug.verdict === -1 && !stale) {
    const expect = s.plug.expect_w === null ? '' : ` (speed ${s.speed} draws ${s.plug.expect_w.toFixed(1)} W)`;
    const looks =
      s.plug.implied_spd >= 0 ? ` That is closer to speed ${s.plug.implied_spd}.` : '';
    warn.textContent =
      `The watt meter reads ${s.plug.w.toFixed(1)} W, which does not match ` +
      `${s.speed > 0 ? `speed ${s.speed}` : 'off'}${expect} — the fan may not be following ` +
      `commands.${looks} Check the control cable at the fan port.`;
    show(warn, true);
  } else {
    show(warn, false);
  }

  paintPwm();
  paintHero();
  paintBits();
  if (view.screen === 'settings' && settingsPainter) settingsPainter();
}

