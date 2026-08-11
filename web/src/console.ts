// Painters for the console screen: hero numbers, gauge, PWM readouts, the
// scope, charts, odometer tiles and status bits. Everything reads the shared
// view-model in state.ts and writes only to the DOM.
//
// The one thing this module does NOT know is the settings screen -- that
// needs the command functions in app.ts, which imports us. app.ts registers
// its settings painter through setSettingsPainter to keep the dependency one
// way.

import {
  SERIES_COLOURS,
  drawAxis,
  drawBattery,
  drawFanSpeed,
  drawSimple,
  drawTemperature,
} from './charts.js';
import { $, at, el, show } from './dom.js';
import { ago, airflow, cardTight, clock, hoursMinutes, signed, storage } from './format.js';
import { drawScope, msPerDivision, type Waveform } from './pwm.js';
import { ROW_IDS, STATUS_BITS, sampleIndex, view, type RowKey } from './state.js';
import { AC, DIM, OK, OR, OUT, PU, TX } from './theme.js';
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
    const tNow = series.ts(series.n - 1);
    // Real timestamp difference, not index * interval: a cache-merged series
    // has holes, and across one the index arithmetic understates the age.
    const minutes =
      t !== null && tNow !== null
        ? (tNow - t) / 60
        : ((series.n - 1 - i) * (view.history?.interval_s ?? 300)) / 60;
    stamp.textContent = `${t === null ? '–' : clock(t)} · ${ago(minutes)}`;
    stamp.className = 'scrub';
  } else {
    stamp.textContent = 'NOW';
    stamp.className = '';
  }

  $('reason').textContent = reason(delta, scrubbing, i);
}

function reason(delta: number | null, scrubbing: boolean, i: number): string {
  const s = view.state;
  if (!s) return '';
  const rest = s.auto_min > 0 ? `speed ${s.auto_min}` : 'off';

  if (scrubbing && view.series) {
    const t = view.series.ts(i);
    const when = t === null ? 'that sample' : clock(t);
    const histSpeed = view.series.spd.length ? view.series.spd[i] : undefined;
    const fanWas =
      histSpeed === undefined
        ? 'not logged'
        : histSpeed > 0
          ? `running at ${histSpeed}`
          : 'off';
    const gap = delta === null ? '–' : delta.toFixed(1);
    return `At ${when} the garage was ${gap}°F hotter than the yard and the fan was ${fanWas}. Move off the chart to return to now.`;
  }
  if (delta === null) {
    const topic = view.info?.topic_out ? ` on ${view.info.topic_out}` : '';
    return `No outdoor reading yet — auto holds the last speed rather than guessing. The yard temperature arrives over MQTT${topic}.`;
  }
  if (!s.auto) {
    return `Auto is off — the fan is at ${s.speed > 0 ? `speed ${s.speed}` : 'off'} because you set it by hand. Turn auto back on to let the differential drive it again.`;
  }
  const gap = delta.toFixed(1);
  if (delta >= s.on_f) {
    return `Garage is ${gap}°F hotter than the yard — past the +${s.on_f}° engage point, so auto is holding speed ${s.auto_max}. It falls back to ${rest} when the gap drops under +${s.off_f}°.`;
  }
  if (delta <= s.off_f) {
    return `Garage is only ${gap}°F hotter than the yard — under the +${s.off_f}° release point, so auto has dropped the fan to ${rest}. It engages speed ${s.auto_max} again above +${s.on_f}°.`;
  }
  return `Gap is ${gap}°F, inside the +${s.off_f}°/+${s.on_f}° deadband — auto is holding ${s.speed > 0 ? `speed ${s.speed}` : 'off'} until it crosses a threshold, so the fan does not chatter.`;
}

function bitValues(): { value: string; colour: string }[] {
  const s = view.state;
  if (!s) return [];
  const card = s.sd_total_mb
    ? storage(s.sd_used_mb, s.sd_total_mb, s.sd_free_mb)
    : s.sd_q
      ? 'quarantined'
      : 'not mounted';
  // A card with no room left stops being a flight recorder, so it reads as a
  // warning rather than as ordinary status text.
  const cardTense = s.sd_total_mb > 0 && cardTight(s.sd_used_mb, s.sd_total_mb);
  let cardColour = OUT;
  if (!s.sd_total_mb) cardColour = DIM;
  else if (cardTense) cardColour = OR;
  return [
    { value: `${s.rssi} dBm`, colour: s.rssi > -70 ? OK : s.rssi > -80 ? OR : '#e0a9a9' },
    { value: card, colour: cardColour },
    { value: s.batt ? `${s.batt.v.toFixed(3)} V` : 'no pack', colour: s.batt ? PU : DIM },
    { value: `${s.toff > 0 ? '+' : ''}${s.toff.toFixed(1)} °C`, colour: OR },
    { value: hoursMinutes(s.uptime_s), colour: OUT },
    {
      value: `${s.slot} · ${s.confirmed ? 'confirmed' : 'unconfirmed'}`,
      colour: s.confirmed ? OK : OR,
    },
  ];
}

export function paintBits(): void {
  const values = bitValues();
  if (!values.length) return;
  const host = $('stats');
  host.replaceChildren(
    ...STATUS_BITS.map((bit, n) => {
      const span = el('span', { className: 'bit' });
      span.append(el('b', { textContent: bit.key }));
      const v = el('span', { textContent: values[n]?.value ?? '' });
      v.style.color = values[n]?.colour ?? DIM;
      span.append(v);
      span.onmouseenter = () => {
        view.tip = n;
        paintTip();
      };
      span.onmouseleave = () => {
        view.tip = -1;
        paintTip();
      };
      span.onclick = () => {
        view.tip = view.tip === n ? -1 : n;
        paintTip();
      };
      return span;
    }),
  );
  paintTip();
}

export function paintTip(): void {
  const bit = view.tip >= 0 ? STATUS_BITS[view.tip] : undefined;
  show($('tip'), !!bit);
  if (!bit) return;
  const title = $('tipt');
  title.textContent = bit.title;
  title.style.color = bitValues()[view.tip]?.colour ?? DIM;
  $('tipb').textContent = bit.body;
}

function paintReadouts(): void {
  const s = view.series;
  const i = sampleIndex();
  if (!s || i < 0) return;
  const tf = at(s.tf, i);
  const of = at(s.of, i);
  $('roT').textContent =
    (tf === null ? '–' : `${tf.toFixed(1)}°`) + (of === null ? '' : ` / out ${of.toFixed(1)}°`);
  const spd = s.spd.length ? s.spd[i] : undefined;
  $('roS').textContent = spd === undefined ? '' : spd > 0 ? `speed ${spd}` : 'off';
  const rh = at(s.rh, i);
  $('roH').textContent = rh === null ? '' : `${rh.toFixed(0)}%`;
  const hpa = at(s.hpa, i);
  $('roP').textContent = hpa === null ? '' : `${hpa.toFixed(1)} mb`;
  const bv = at(s.bv, i);
  $('roB').textContent =
    bv === null ? '' : `${bv.toFixed(2)} V${s.chg[i] === 1 ? ' ⚡ charging' : ''}`;
}

export function drawAll(): void {
  if (view.screen !== 'console') return;
  const s = view.series;
  if (!s) {
    // No series and a reason for it: blank the plots and say why, rather than
    // leaving the previous range's picture under the new range's title.
    if (view.historyError) {
      $('tcap').textContent = view.historyError;
      for (const id of ['cv_t', 'cv_s', 'cv_h', 'cv_p', 'cv_b', 'cv_ax']) {
        const c = document.getElementById(id) as HTMLCanvasElement | null;
        const ctx = c?.getContext('2d');
        if (c && ctx) ctx.clearRect(0, 0, c.width, c.height);
      }
    }
    return;
  }
  drawTemperature($<HTMLCanvasElement>('cv_t'), s, view.scrub);
  if (view.rows.fan) drawFanSpeed($<HTMLCanvasElement>('cv_s'), s, view.scrub);
  if (view.rows.humidity) {
    drawSimple($<HTMLCanvasElement>('cv_h'), s, s.rh, SERIES_COLOURS.humidity,
      (v) => v.toFixed(0), 'no humidity data', view.scrub);
  }
  if (view.rows.pressure) {
    // 0.5 hPa floor: the ticks carry one decimal, so a smaller range is still
    // legible in the labels and does not need flattening.
    drawSimple($<HTMLCanvasElement>('cv_p'), s, s.hpa, SERIES_COLOURS.pressure,
      (v) => v.toFixed(1), 'no pressure data', view.scrub, 0.5);
  }
  if (view.rows.battery) drawBattery($<HTMLCanvasElement>('cv_b'), s, view.scrub);
  drawAxis($<HTMLCanvasElement>('cv_ax'), s, view.days);
  paintReadouts();
}

export function paintStats(): void {
  const st = view.stats;
  if (!st) return;
  $('mW').textContent = `${st.watts_now.toFixed(0)} W`;
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
    b.style.color = on ? TX : '#4d5765';
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

export function paint(next?: DeviceState): void {
  if (next) {
    view.lastOk = Date.now();
    view.offline = false;
    // A straggler still proves the device is reachable, so it clears `offline`
    // -- but it must not be allowed to skip the repaint, or the page keeps
    // showing "● NO CONTACT" and stays dimmed while the state says otherwise,
    // for up to a full poll cycle. Drop only the stale VALUES, not the render.
    if (isNewer(next, view.state)) view.state = next;
  }
  const s = view.state;
  if (!s) return;

  const stale = view.offline;
  const mqtt = $('hmq');
  // Offline outranks every field in here: they are all last-known values, and
  // the broker flag in particular would otherwise keep asserting "UP" about a
  // device we have not heard from in minutes.
  mqtt.textContent = stale ? '● NO CONTACT' : s.mqtt ? '● BROKER UP' : '● BROKER DOWN';
  mqtt.className = stale ? 'stale' : s.mqtt ? 'up' : '';
  document.body.classList.toggle('offline', stale);
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

  $('railnum').textContent = s.speed === 0 ? 'off' : s.speed < 0 ? 'raw' : String(s.speed);
  Array.from($('stack').children).forEach((child, k) => {
    const b = child as HTMLElement;
    const n = k + 1;
    const lit = n <= s.speed && s.speed > 0;
    b.style.background = lit ? `rgba(59,130,246,${0.2 + 0.055 * n})` : '#12161d';
    b.style.borderColor = n === s.speed ? AC : lit ? 'rgba(59,130,246,.4)' : '#1a2029';
  });

  $('bauto').className = `pill${s.auto ? ' on' : ''}`;
  $('bauto').textContent = s.auto ? 'auto on' : 'auto off';
  $('boff').className = `pill${s.speed === 0 ? ' on' : ''}`;
  $('mAir').textContent = airflow(s.speed);

  paintPwm();
  paintHero();
  paintBits();
  if (view.screen === 'settings' && settingsPainter) settingsPainter();
}

