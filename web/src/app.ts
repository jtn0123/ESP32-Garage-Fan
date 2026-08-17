// The console controller.
//
// One mutable `view` object, and paint functions that re-derive the DOM from
// it. State arrives by SSE (or the 15 s poll fallback) and every command
// endpoint answers with the new state, so nothing here optimistically guesses
// what the device did.

import * as api from './api.js';
import {
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
import { drawAll, paintCaption, paintChartTitle } from './history_view.js';
import { buildRail } from './rail.js';
import { attachScrub, endScrub } from './scrub.js';
import { drawPreview, drawScope } from './pwm.js';
import { build } from './series.js';
import { buildGroups, render as renderSettings } from './settings.js';
import { ROW_IDS, view, type RowKey } from './state.js';
import type { DeviceState } from './types.js';
import { maintenance, uploadFirmware } from './ota.js';
import { checkForUpdate } from './update.js';




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
  // Through the module that owns the crosshair, so the height reserved for the
  // state sentence during a gesture is released with it (scrub.ts).
  endScrub();
  Array.from($('ranges').children).forEach((child) => {
    const b = child as HTMLElement;
    b.className = Number(b.dataset['d']) === days ? 'on' : '';
  });
  // Title and caption both live in history_view now: the title slot doubles as
  // the scrub readout, and the caption has to be able to say "this range is
  // short" or "this range failed" rather than only carrying the standing hint.
  paintChartTitle();
  paintCaption();
  void loadHistory();
}

/* --------------------------------------------------------------------- setup */

/**
 * Does the chip strip continue past the right edge?
 *
 * The class drives the edge fade. Kept in sync from the scroll position rather
 * than assumed from the width, because the answer changes as you scroll and a
 * fade that stays put once you have reached NOX is a lie about there being
 * more.
 */
function markChipOverflow(host: HTMLElement): void {
  const more = host.scrollLeft + host.clientWidth < host.scrollWidth - 2;
  host.classList.toggle('more', more);
}

function buildChips(): void {
  const host = $('chips');
  host.addEventListener('scroll', () => markChipOverflow(host));
  window.addEventListener('resize', () => markChipOverflow(host));
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
  markChipOverflow(host);
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
    // Boots ride along but must never be able to fail the chart: an older
    // firmware has no /api/boots, and a missing explanation is not a reason
    // to withhold the data it would have explained.
    const [raw, boots] = await Promise.all([
      api.getHistory(view.days),
      api.getBoots(view.days).catch(() => ({ boots: [] })),
    ]);
    if (seq !== historySeq) return; // superseded by a newer range request
    view.historyError = null;
    view.history = raw;
    view.boots = boots.boots ?? [];
    view.series = build(raw);
    drawAll();
    paintHero();
  } catch (err) {
    if (seq !== historySeq) return;
    // Do NOT keep the previous chart. The firmware answers 503 for a range it
    // cannot serve (card unmounted, clock unsynced) precisely so the caller is
    // not handed a plausible substitute -- and silently retaining the old
    // series undid that at the last step: picking 30D with no card relabelled
    // the 7-day chart "LAST 30 DAYS" and showed nothing else. Say what
    // happened and draw nothing.
    view.historyError =
      err instanceof Error && /50\d/.test(err.message)
        ? 'this range is unavailable — the card is not mounted, or the clock has not synced'
        : 'could not load history from the controller';
    view.history = null;
    view.boots = [];
    view.series = null;
    drawAll();
    paintHero();
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

// Two missed polls plus a margin. 45 s was three missed polls, which meant a
// page resumed from background in front of a dead controller showed a
// confident 74.9 under a `NOW` badge for most of a minute -- and in a garage
// that is exactly the reading someone acts on. The decay is visible before the
// verdict lands: the badge switches to the age of the last answer on the FIRST
// failure (paintHero), so nothing claims to be current in the meantime.
const OFFLINE_AFTER_MS = 20_000;

async function poll(): Promise<void> {
  try {
    paint(await api.getState());
  } catch {
    // The catch used to be entirely empty, which is how a dead device kept
    // rendering as a live one. Failing is information; record it.
    view.pollFail += 1;
    // `lastOk || startedAt`: the old guard was `if (view.lastOk && ...)`, and
    // lastOk is 0 until a poll succeeds -- so a console opened COLD against a
    // dead controller could never reach the offline state at all. Measuring
    // from page load instead makes the cold case reach a stated failure.
    const since = view.lastOk || view.startedAt;
    if (Date.now() - since > OFFLINE_AFTER_MS) view.offline = true;
    paint();
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
  buildRail(setSpeed);
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

  attachScrub($('plots'));
  // Tap anywhere else to dismiss an open explanation. On a phone the tip has no
  // mouseleave to close it, so without this the only way out was finding the
  // same bit again. The `.bit` guard matters: this listener sees the same click
  // that just opened the tip, on its way up to the document.
  document.addEventListener('click', (e) => {
    if (view.tip < 0) return;
    if ((e.target as HTMLElement | null)?.closest('#stats .bit')) return;
    view.tip = -1;
    paintTip();
  });
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
