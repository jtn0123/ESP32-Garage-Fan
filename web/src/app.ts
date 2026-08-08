// The console controller.
//
// One mutable `view` object, and paint functions that re-derive the DOM from
// it. State arrives by SSE (or the 15 s poll fallback) and every command
// endpoint answers with the new state, so nothing here optimistically guesses
// what the device did.

import * as api from './api.js';
import {
  SERIES_COLOURS,
  drawAxis,
  drawBattery,
  drawFanSpeed,
  drawSimple,
  drawTemperature,
} from './charts.js';
import { $, at, clear, el, show } from './dom.js';
import { ago, airflow, clock, hoursMinutes, signed, storage } from './format.js';
import { drawPreview, drawScope, msPerDivision, type Waveform } from './pwm.js';
import { build, mergeCache, type Series } from './series.js';
import { buildGroups, render as renderSettings } from './settings.js';
import { AC, DIM, OK, OR, OUT, PAD_LEFT as L, PAD_RIGHT as R, PU, TX } from './theme.js';
import type { DeviceInfo, DeviceState, History, Sensors, Stats } from './types.js';
import { checkForUpdate, type UpdateStatus } from './update.js';

type RowKey = 'fan' | 'humidity' | 'pressure' | 'battery';

interface View {
  state: DeviceState | null;
  info: DeviceInfo | null;
  stats: Stats | null;
  live: Sensors | null;
  series: Series | null;
  history: History | null;
  update: UpdateStatus | null;
  days: number;
  scrub: number;
  screen: 'console' | 'settings';
  scopeOpen: boolean;
  previewOpen: boolean;
  tip: number;
  rows: Record<RowKey, boolean>;
  phase: number;
  raf: number | null;
}

const view: View = {
  state: null,
  info: null,
  stats: null,
  live: null,
  series: null,
  history: null,
  update: null,
  days: 1,
  scrub: -1,
  screen: 'console',
  scopeOpen: false,
  previewOpen: false,
  tip: -1,
  rows: { fan: true, humidity: false, pressure: false, battery: false },
  phase: 0,
  raf: null,
};

const ROW_IDS: Record<RowKey, { sub: string; canvas: string; readout: string }> = {
  fan: { sub: 'sub_s', canvas: 'cv_s', readout: 'roS' },
  humidity: { sub: 'sub_h', canvas: 'cv_h', readout: 'roH' },
  pressure: { sub: 'sub_p', canvas: 'cv_p', readout: 'roP' },
  battery: { sub: 'sub_b', canvas: 'cv_b', readout: 'roB' },
};

const STATUS_BITS = [
  {
    key: 'RSSI',
    title: 'WI-FI SIGNAL',
    body: 'How strong the controller’s link to the house access point is. Around −61 dBm is a solid connection; below about −80 dBm the MQTT session starts dropping and telemetry gaps show up in the charts.',
  },
  {
    key: 'SD',
    title: 'LOG STORAGE',
    body: 'Space used by on-device logs on the microSD card. Every 5-minute sample is written locally first, then published — so a broker outage never loses history. The card is mounted lazily and quarantined if a mount ever crashes the board.',
  },
  {
    key: 'VBAT',
    title: 'BATTERY VOLTAGE',
    body: 'Live cell voltage of the backup pack — the same series plotted in the BATTERY row. Roughly 4.10 V is full and 3.3 V is empty. The pack keeps the controller logging when the USB supply drops out; the fan itself runs from its own 12 V brick.',
  },
  {
    key: 'T-OFFSET',
    title: 'PROBE CALIBRATION',
    body: 'A fixed correction added to every garage temperature reading to cancel heat the board adds to its own sensor. Charging warms the board much more than idle does, so there are two values — set both in Settings › Sensors after comparing against a reference thermometer.',
  },
  {
    key: 'UPTIME',
    title: 'TIME SINCE BOOT',
    body: 'How long the controller has been running without a reboot. A number that keeps resetting to zero usually means brownouts on the supply rather than a firmware crash — the status line also carries the previous boot’s cause of death.',
  },
  {
    key: 'SLOT',
    title: 'FIRMWARE SLOT',
    body: 'The device keeps two firmware images, app0 and app1, and boots whichever was flashed last. “Confirmed” means this image came up cleanly and checked in with the broker, so it is now the permanent choice — an unconfirmed image rolls back to the other slot rather than boot-looping.',
  },
] as const;

/* ------------------------------------------------------------------ waveform */

function waveform(): Waveform {
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

function startScope(): void {
  drawScope($<HTMLCanvasElement>('cv_pw'), waveform(), view.phase, performance.now());
  if (view.raf === null) view.raf = requestAnimationFrame(scopeTick);
}

function stopScope(): void {
  if (view.raf !== null) {
    cancelAnimationFrame(view.raf);
    view.raf = null;
  }
}

/* ------------------------------------------------------------------- painting */

function sampleIndex(): number {
  const s = view.series;
  if (!s || !s.n) return -1;
  return view.scrub >= 0 ? view.scrub : s.n - 1;
}

function paintPwm(): void {
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

function paintHero(): void {
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
    const minutes = ((series.n - 1 - i) * (view.history?.interval_s ?? 300)) / 60;
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
    ? storage(s.sd_used_mb, s.sd_total_mb)
    : s.sd_q
      ? 'quarantined'
      : 'not mounted';
  return [
    { value: `${s.rssi} dBm`, colour: s.rssi > -70 ? OK : s.rssi > -80 ? OR : '#e0a9a9' },
    { value: card, colour: s.sd_total_mb ? OUT : DIM },
    { value: s.batt ? `${s.batt.v.toFixed(3)} V` : 'no pack', colour: s.batt ? PU : DIM },
    { value: `${s.toff > 0 ? '+' : ''}${s.toff.toFixed(1)} °C`, colour: OR },
    { value: hoursMinutes(s.uptime_s), colour: OUT },
    {
      value: `${s.slot} · ${s.confirmed ? 'confirmed' : 'unconfirmed'}`,
      colour: s.confirmed ? OK : OR,
    },
  ];
}

function paintBits(): void {
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

function paintTip(): void {
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

function drawAll(): void {
  if (view.screen !== 'console') return;
  const s = view.series;
  if (!s) return;
  drawTemperature($<HTMLCanvasElement>('cv_t'), s, view.scrub);
  if (view.rows.fan) drawFanSpeed($<HTMLCanvasElement>('cv_s'), s, view.scrub);
  if (view.rows.humidity) {
    drawSimple($<HTMLCanvasElement>('cv_h'), s, s.rh, SERIES_COLOURS.humidity,
      (v) => v.toFixed(0), 'no humidity data', view.scrub);
  }
  if (view.rows.pressure) {
    drawSimple($<HTMLCanvasElement>('cv_p'), s, s.hpa, SERIES_COLOURS.pressure,
      (v) => v.toFixed(1), 'no pressure data', view.scrub);
  }
  if (view.rows.battery) drawBattery($<HTMLCanvasElement>('cv_b'), s, view.scrub);
  drawAxis($<HTMLCanvasElement>('cv_ax'), s, view.days);
  paintReadouts();
}

function paintStats(): void {
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

export function paint(next?: DeviceState): void {
  if (next) view.state = next;
  const s = view.state;
  if (!s) return;

  const mqtt = $('hmq');
  mqtt.textContent = s.mqtt ? '● BROKER UP' : '● BROKER DOWN';
  mqtt.className = s.mqtt ? 'up' : '';
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
  if (view.screen === 'settings') paintSettings();
}

function paintSettings(): void {
  if (!view.state) {
    clear($('groups'));
    return;
  }
  renderSettings(
    $('groups'),
    buildGroups({
      state: view.state,
      info: view.info,
      update: view.update,
      setConfig: (q) => void command(() => api.setConfig(q)),
      toggleAuto: () => void command(() => api.setConfig(`auto=${view.state?.auto ? 0 : 1}`)),
      restart: () => void maintenance('restart'),
      formatCard: () => void maintenance('format'),
      recheckUpdate: () => void runUpdateCheck(true),
    }),
  );
  const go = document.getElementById('ota_go');
  if (go) go.onclick = () => void uploadFirmware();
}

/* ------------------------------------------------------------------ commands */

async function command(run: () => Promise<DeviceState>): Promise<void> {
  try {
    paint(await run());
  } catch {
    /* the 15 s poll re-syncs; a transient failure should not blank the page */
  }
}

export function setSpeed(n: number): void {
  void command(() => api.setSpeed(n));
}

function tokenFromFieldOrPrompt(question: string): string | null {
  const field = document.getElementById('ota_t') as HTMLInputElement | null;
  if (field?.value) return field.value;
  return window.prompt(question);
}

async function maintenance(kind: 'restart' | 'format'): Promise<void> {
  const token = tokenFromFieldOrPrompt(
    kind === 'restart'
      ? 'Update token to restart the controller:'
      : 'Update token to format the card:',
  );
  if (!token) return;
  const confirmed = window.confirm(
    kind === 'restart'
      ? 'Reboot the controller? The fan keeps its current speed through the reset.'
      : 'Formatting erases every sample stored on the card. Continue?',
  );
  if (!confirmed) return;
  try {
    window.alert(await (kind === 'restart' ? api.restart(token) : api.formatCard(token)));
  } catch (err) {
    window.alert(`request failed: ${err instanceof Error ? err.message : String(err)}`);
  }
}

async function uploadFirmware(): Promise<void> {
  const file = (document.getElementById('ota_f') as HTMLInputElement | null)?.files?.[0];
  const token = (document.getElementById('ota_t') as HTMLInputElement | null)?.value ?? '';
  const msg = document.getElementById('otamsg');
  if (!msg) return;
  if (!file) {
    msg.textContent = 'pick firmware.bin first';
    return;
  }
  msg.textContent = `uploading ${(file.size / 1024).toFixed(0)} KB…`;
  try {
    msg.textContent = await api.uploadFirmware(file, token);
  } catch (err) {
    msg.textContent = `upload failed: ${err instanceof Error ? err.message : String(err)}`;
  }
}

/* -------------------------------------------------------------- update check */

async function runUpdateCheck(force = false): Promise<void> {
  const repo = view.info?.repo;
  const running = view.state?.fw;
  if (!repo || !running) return;
  if (view.update && !force) return; // one check per page load; the button forces
  view.update = await checkForUpdate(repo, running);
  if (view.screen === 'settings') paintSettings();
  const s = view.update;
  show($('updot'), s.kind === 'available');
}

/* ---------------------------------------------------------------- navigation */

export function setScreen(next?: 'console' | 'settings'): void {
  view.screen = next ?? (view.screen === 'console' ? 'settings' : 'console');
  show($('console'), view.screen === 'console');
  show($('settings'), view.screen === 'settings');
  const nav = $('nav');
  nav.textContent = view.screen === 'settings' ? '← CONSOLE' : 'SETTINGS';
  nav.className = view.screen === 'settings' ? 'on' : '';
  view.tip = -1;
  paintTip();
  if (view.screen === 'console') {
    drawAll();
    if (view.previewOpen) drawPreview($<HTMLCanvasElement>('cv_pm'), waveform());
    if (view.scopeOpen) startScope();
  } else {
    stopScope();
    paintSettings();
  }
}

function setPreview(open: boolean): void {
  const want = view.scopeOpen ? false : open;
  if (view.previewOpen === want) return;
  view.previewOpen = want;
  show($('pwmcard'), want);
  paintPwm();
  if (want) drawPreview($<HTMLCanvasElement>('cv_pm'), waveform());
}

function toggleScope(ev?: Event): void {
  ev?.stopPropagation();
  view.scopeOpen = !view.scopeOpen;
  view.previewOpen = false;
  show($('pwmcard'), false);
  show($('scope'), view.scopeOpen);
  paintPwm();
  if (view.scopeOpen) startScope();
  else stopScope();
}

function setRange(days: number): void {
  view.days = days;
  view.scrub = -1;
  Array.from($('ranges').children).forEach((child) => {
    const b = child as HTMLElement;
    b.className = Number(b.dataset['d']) === days ? 'on' : '';
  });
  $('chtitle').textContent = days === 1 ? 'LAST 24 HOURS' : `LAST ${days} DAYS`;
  $('tcap').textContent =
    days === 1
      ? 'band = how much hotter the garage is than the yard · drag across to read any moment'
      : 'garage only — the card keeps no outdoor series · drag across to read any moment';
  void loadHistory();
}

/* ----------------------------------------------------------------- scrubbing */

function scrubAt(clientX: number): void {
  const s = view.series;
  if (!s || s.n < 2) return;
  const rect = $<HTMLCanvasElement>('cv_t').getBoundingClientRect();
  const fraction = (clientX - rect.left - L) / (rect.width - L - R);
  const i = Math.round(fraction * (s.n - 1));
  const next = i < 0 || i >= s.n || fraction < -0.02 || fraction > 1.02 ? -1 : i;
  if (next === view.scrub) return;
  view.scrub = next;
  drawAll();
  paintHero();
}

function endScrub(): void {
  if (view.scrub === -1) return;
  view.scrub = -1;
  drawAll();
  paintHero();
}

/* --------------------------------------------------------------------- setup */

function buildSpeedStack(): void {
  const stack = $('stack');
  stack.replaceChildren(
    ...Array.from({ length: 12 }, (_, k) => {
      const n = k + 1;
      return el('button', { title: `set speed ${n}`, onclick: () => setSpeed(n) });
    }),
  );
}

function buildChips(): void {
  const host = $('chips');
  host.replaceChildren(
    ...(Object.keys(ROW_IDS) as RowKey[]).map((key) => {
      const b = el('button', { textContent: key.toUpperCase() });
      b.dataset['key'] = key;
      b.onclick = () => {
        view.rows[key] = !view.rows[key];
        paintChips();
        requestAnimationFrame(drawAll);
      };
      return b;
    }),
  );
  paintChips();
}

function paintChips(): void {
  Array.from($('chips').children).forEach((child) => {
    const b = child as HTMLElement;
    const key = b.dataset['key'] as RowKey;
    const on = view.rows[key];
    b.style.color = on ? TX : '#4d5765';
    b.style.borderColor = on ? SERIES_COLOURS[key] : 'transparent';
    show($(ROW_IDS[key].sub), on);
  });
}

function buildCaptureTable(): void {
  const info = view.info;
  if (!info) return;
  const grid = $('capgrid');
  grid.replaceChildren(
    ...info.high_us.map((us, n) => {
      const b = el('button');
      b.append(
        el('div', { className: 'ck', textContent: n === 0 ? 'OFF' : String(n) }),
        el('div', { className: 'cu', textContent: n === 0 ? '—' : String(us) }),
      );
      b.onclick = (ev) => {
        ev.stopPropagation();
        setSpeed(n);
      };
      return b;
    }),
  );
}

async function loadHistory(): Promise<void> {
  try {
    const raw = await api.getHistory(view.days);
    const merged = view.days === 1 ? mergeCache(raw) : raw;
    view.history = merged;
    view.series = build(merged);
    drawAll();
    paintHero();
  } catch {
    /* keep the last good chart rather than blanking it */
  }
}

async function refreshClimate(): Promise<void> {
  try {
    view.live = await api.getSensors();
  } catch {
    /* the history tail covers the hero reading */
  }
  try {
    view.stats = await api.getStats();
    paintStats();
  } catch {
    /* odometer keeps its last values */
  }
  await loadHistory();
  paintHero();
}

async function poll(): Promise<void> {
  try {
    paint(await api.getState());
  } catch {
    /* SSE or the next poll will catch up */
  }
}

export async function boot(): Promise<void> {
  buildSpeedStack();
  buildChips();

  $('nav').onclick = () => setScreen();
  $('bauto').onclick = () =>
    void command(() => api.setConfig(`auto=${view.state?.auto ? 0 : 1}`));
  $('boff').onclick = () => setSpeed(0);
  const pwmCell = $('pwmcell');
  pwmCell.onmouseenter = () => setPreview(true);
  pwmCell.onmouseleave = () => setPreview(false);
  pwmCell.onclick = () => toggleScope();
  $('scclose').onclick = (ev) => toggleScope(ev);
  Array.from($('ranges').children).forEach((child) => {
    const b = child as HTMLElement;
    b.onclick = () => setRange(Number(b.dataset['d']));
  });

  const plots = $('plots');
  plots.addEventListener('mousemove', (e) => scrubAt((e as MouseEvent).clientX));
  plots.addEventListener('mouseleave', endScrub);
  plots.addEventListener(
    'touchmove',
    (e) => {
      const touch = (e as TouchEvent).touches[0];
      if (!touch) return;
      scrubAt(touch.clientX);
      e.preventDefault();
    },
    { passive: false },
  );
  plots.addEventListener('touchend', endScrub);
  window.addEventListener('resize', () => {
    drawAll();
    if (view.previewOpen) drawPreview($<HTMLCanvasElement>('cv_pm'), waveform());
    if (view.scopeOpen) drawScope($<HTMLCanvasElement>('cv_pw'), waveform(), view.phase, performance.now());
  });

  try {
    view.info = await api.getDevice();
    buildCaptureTable();
  } catch {
    /* the duty table falls back to its defaults; the rest of the page works */
  }

  await poll();
  await refreshClimate();
  void runUpdateCheck();

  api.subscribe(paint);
  window.setInterval(() => void poll(), 15_000);
  window.setInterval(() => void refreshClimate(), 60_000);
}
