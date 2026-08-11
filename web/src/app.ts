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
import { build } from './series.js';
import { buildGroups, render as renderSettings } from './settings.js';
import { ROW_IDS, view, type RowKey } from './state.js';
import { PAD_LEFT as L, PAD_RIGHT as R } from './theme.js';
import type { DeviceState } from './types.js';
import { checkForUpdate, parseChecksum, sha256Hex } from './update.js';




function paintSettings(): void {
  if (!view.state) {
    clear($('groups'));
    return;
  }
  // Never rebuild the settings DOM out from under someone who is using it.
  //
  // renderSettings() calls host.replaceChildren(), and paint() runs on every
  // SSE frame and every 15 s poll. That destroyed and recreated the update
  // token field mid-typing (upload then POSTed token="" and 403'd), stole
  // focus from the +/- steppers so keyboard repeat died after one press, and
  // detached the OTA progress node so a ~1 MB upload -- which always outlives
  // 15 s -- reported neither success nor failure and looked hung forever.
  //
  // The OTA row is additionally kept as a stable node (see otaControl), so an
  // upload in flight survives even a rebuild triggered by leaving and
  // returning to the screen.
  // Only an element whose CONTENT is being edited blocks the repaint. Keying
  // on "focus is anywhere inside #groups" was too broad: a stepper button or a
  // toggle pill keeps focus after a click, which would freeze every later
  // repaint and leave the whole screen showing stale values until focus moved
  // out of the host.
  const host = $('groups');
  const active = document.activeElement as HTMLElement | null;
  const editing =
    !!active &&
    host.contains(active) &&
    (active instanceof HTMLInputElement ||
      active instanceof HTMLTextAreaElement ||
      active instanceof HTMLSelectElement ||
      active.isContentEditable);
  if (editing) return;

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
      purgeCard: () => void maintenance('purge'),
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

const MAINTENANCE = {
  restart: {
    prompt: 'Update token to restart the controller:',
    confirm: 'Reboot the controller? The fan keeps its current speed through the reset.',
    run: api.restart,
  },
  format: {
    prompt: 'Update token to format the card:',
    confirm: 'Formatting erases every sample stored on the card. Continue?',
    run: api.formatCard,
  },
  purge: {
    prompt: 'Update token to delete the card contents:',
    // Names what is actually at stake: this deletes EVERYTHING on the card,
    // including whatever was on it before the fan ever used it.
    confirm:
      'Delete every file on the SD card? This unlinks all contents — the fan’s logs AND anything else stored on the card — leaving the filesystem in place. It cannot be undone.',
    run: api.purgeCard,
  },
} as const;

async function maintenance(kind: keyof typeof MAINTENANCE): Promise<void> {
  const op = MAINTENANCE[kind];
  const token = tokenFromFieldOrPrompt(op.prompt);
  if (!token) return;
  if (!window.confirm(op.confirm)) return;
  try {
    window.alert(await op.run(token));
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
  // Integrity check before anything is written to flash.
  //
  // There is no secure boot and /update accepts whatever bytes it is handed,
  // so the release's .sha256 sidecar is the only thing standing between
  // GitHub and the flash -- and until now nothing consulted it. If the running
  // update check found an installable asset we fetch its sidecar and refuse a
  // mismatch outright. When the sidecar cannot be fetched (GitHub's asset host
  // may not allow the cross-origin read) we still show the computed digest so
  // it can be compared by eye rather than silently skipping the check.
  msg.textContent = 'checking image…';
  let digest: string | null = null;
  try {
    digest = await sha256Hex(file);
  } catch (err) {
    // Fail closed. Hashing is the only integrity check between GitHub and the
    // flash, and "we could not hash it" is not a reason to write it anyway.
    msg.textContent = `ABORTED: could not hash the image (${err instanceof Error ? err.message : String(err)}).`;
    return;
  }
  const upd = view.update;
  const asset = upd?.kind === 'available' ? upd.asset : null;
  const latest = upd && 'latest' in upd ? upd.latest : 'the release';
  const short = `${digest.slice(0, 16)}…`;
  // Say plainly whether anything was COMPARED. Printing a digest on its own
  // reads like a passed check, and in the common cases -- a locally built
  // image, or a check that came back ahead/no-binary/up-to-date -- there is no
  // published sum to compare it against and nothing was verified at all.
  if (asset) {
    const expected = await fetch(`${asset.browser_download_url}.sha256`)
      .then((r) => (r.ok ? r.text() : null))
      .then((t) => (t ? parseChecksum(t) : null))
      .catch(() => null);
    if (expected && expected !== digest) {
      msg.textContent = `ABORTED: this file's SHA-256 does not match ${latest}. Do not flash it.`;
      return;
    }
    msg.textContent = expected
      ? `verified against ${latest} (sha256 ${short})`
      : `NOT VERIFIED — could not fetch ${latest}'s published sum; sha256 ${short}`;
  } else {
    msg.textContent = `NOT VERIFIED — no published checksum to compare; sha256 ${short}`;
  }

  const size = `${(file.size / 1024).toFixed(0)} KB`;
  msg.textContent = `${msg.textContent ? `${msg.textContent} — ` : ''}uploading ${size}…`;
  try {
    const body = await api.uploadFirmware(file, token);
    // The endpoint answers JSON for both outcomes; surface the failure as one.
    msg.textContent = /"error"/.test(body) ? `upload failed: ${body}` : body;
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

// Monotonic request id. Range switching used to have no sequencing at all:
// the handler read view.days again AFTER the await, so clicking 24H then 7D
// quickly could let the 24 h response land last and be drawn under the 7-day
// title, with the axis formatted for a week. A late response for a range the
// user has already left is now discarded rather than rendered.
let historySeq = 0;

async function loadHistory(): Promise<void> {
  const seq = ++historySeq;
  try {
    const raw = await api.getHistory(view.days);
    if (seq !== historySeq) return; // superseded by a newer range request
    view.history = raw;
    view.series = build(raw);
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

// Three missed polls. Long enough that one dropped request or a brief WiFi
// flap does not cry wolf, short enough that a page left open on a dead
// controller stops presenting its last readings as current.
const OFFLINE_AFTER_MS = 45_000;

async function poll(): Promise<void> {
  try {
    paint(await api.getState());
  } catch {
    // The catch used to be entirely empty, which is how a dead device kept
    // rendering as a live one. Failing is information; record it.
    if (view.lastOk && Date.now() - view.lastOk > OFFLINE_AFTER_MS && !view.offline) {
      view.offline = true;
      paint();
    }
  }
}

/**
 * Fetch the immutable device facts, retrying until they arrive.
 *
 * This used to be a single attempt at boot whose failure was swallowed with a
 * comment claiming "the duty table falls back to its defaults". There is no
 * such fallback: console.ts reads `view.info?.high_us[speed] ?? 0`, so one
 * timed-out request (10 s abort, reachable on a weak link) left the PWM DUTY
 * tile reading 0.0%, the scope badge showing IDLE and the capture grid empty
 * while the gate pin was actually transmitting -- permanently, because nothing
 * ever asked again. It also stranded the update check, which returns early
 * without `info.repo`, leaving the UPDATE row on "checking…" with a "Check
 * again" button that could not do anything.
 */
async function loadDevice(attempt = 0): Promise<void> {
  try {
    view.info = await api.getDevice();
    buildCaptureTable();
    paintPwm();
    // The update check needs info.repo and returns early without it, so a
    // late-arriving device fetch has to nudge it. Guarded internally against
    // running twice.
    void runUpdateCheck();
    if (view.screen === 'settings') paintSettings();
  } catch {
    // Back off to a minute and keep trying; these facts never change, so a
    // late answer is still the right answer.
    const delay = Math.min(60_000, 2_000 * 2 ** attempt);
    window.setTimeout(() => void loadDevice(attempt + 1), delay);
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

  await loadDevice();

  await poll();
  await refreshClimate();
  void runUpdateCheck();

  api.subscribe(paint);
  window.setInterval(() => void poll(), 15_000);
  window.setInterval(() => void refreshClimate(), 60_000);
}
