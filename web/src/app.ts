// The console controller.
//
// One mutable `view` object, and paint functions that re-derive the DOM from
// it. State arrives by SSE (or the 15 s poll fallback) and every command
// endpoint answers with the new state, so nothing here optimistically guesses
// what the device did.

import * as api from './api.js';
import {
  drawAll,
  paint,
  paintChips,
  paintHero,
  paintPwm,
  paintStats,
  paintTip,
  setSettingsPainter,
  startScope,
  stopScope,
  waveform,
} from './console.js';
import { $, clear, el, show } from './dom.js';
import { drawPreview, drawScope } from './pwm.js';
import { build, mergeCache } from './series.js';
import { buildGroups, render as renderSettings } from './settings.js';
import { ROW_IDS, view, type RowKey } from './state.js';
import { PAD_LEFT as L, PAD_RIGHT as R } from './theme.js';
import type { DeviceState } from './types.js';
import { checkForUpdate } from './update.js';




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
  // Nearest sample by its time position, not index arithmetic: around a gap
  // the two disagree, and the crosshair must land on a sample that exists.
  let i = 0;
  let best = Infinity;
  for (let k = 0; k < s.n; k++) {
    const d = Math.abs((s.frac[k] ?? 0) - fraction);
    if (d < best) {
      best = d;
      i = k;
    }
  }
  const next = fraction < -0.02 || fraction > 1.02 ? -1 : i;
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
    let hist = raw;
    let stamps: number[] | null = null;
    if (view.days === 1) {
      const merged = mergeCache(raw);
      hist = merged.h;
      stamps = merged.ts; // the union's real epochs -- holes and all
    }
    view.history = hist;
    view.series = build(hist, stamps ?? undefined);
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
  setSettingsPainter(paintSettings);
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
