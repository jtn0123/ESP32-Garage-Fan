// The settings screen: a declarative spec plus a renderer.
//
// Every control here maps to something the firmware actually exposes. The
// design mockup this came from also drew overnight-quiet, PWM-floor, stall
// detection, solar priority and an update channel -- none of which exist in
// the firmware, and two of which the hardware cannot support (there is no tach
// line; D- was measured idle). Shipping controls that silently do nothing is
// worse than not shipping them, so those rows are absent rather than inert.

import * as api from './api.js';
import { el, show } from './dom.js';
import { hoursMinutes, storage } from './format.js';
import type { DeviceInfo, DeviceState } from './types.js';
import { ageText, paintFrame } from './panel.js';
import type { UpdateStatus } from './update.js';

export type Row =
  | { kind: 'step'; label: string; hint: string; value: string; dec: () => void; inc: () => void }
  | { kind: 'toggle'; label: string; hint: string; on: boolean; toggle: () => void }
  | { kind: 'text'; label: string; hint: string; value: string }
  | { kind: 'actions'; label: string; hint: string; actions: Action[] }
  | { kind: 'update'; label: string; hint: string; status: UpdateStatus | null; recheck: () => void }
  | { kind: 'ota'; label: string; hint: string }
  | { kind: 'panel'; label: string; hint: string };

export interface Action {
  text: string;
  href?: string;
  run?: () => void;
  danger?: boolean;
}

export interface Group {
  title: string;
  blurb: string;
  rows: Row[];
}

export interface SettingsDeps {
  state: DeviceState;
  info: DeviceInfo | null;
  update: UpdateStatus | null;
  setConfig: (query: string) => void;
  toggleAuto: () => void;
  restart: () => void;
  formatCard: () => void;
  purgeCard: () => void;
  recheckUpdate: () => void;
}

const clamp = (v: number, lo: number, hi: number): number =>
  Math.min(hi, Math.max(lo, Number(v.toFixed(2))));

export function buildGroups(d: SettingsDeps): Group[] {
  const s = d.state;
  const info = d.info;
  const set = d.setConfig;
  const eng = s.on_f;
  const rel = s.off_f;

  const step = (
    label: string,
    hint: string,
    value: string,
    dec: () => void,
    inc: () => void,
  ): Row => ({ kind: 'step', label, hint, value, dec, inc });

  const text = (label: string, hint: string, value: string): Row =>
    ({ kind: 'text', label, hint, value });

  return [
    {
      title: 'AUTO MODE',
      blurb:
        'The differential band that decides when the fan runs on its own. A wider band means fewer start/stop cycles.',
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
      ],
    },
    {
      title: 'SENSORS',
      blurb:
        'Calibration and sampling. Every stored reading is corrected before it is written, so changes here do not rewrite history.',
      rows: [
        step(
          'Probe offset · charging',
          'Correction added to the garage sensor while the pack is charging, when the board runs warmest.',
          `${s.offc > 0 ? '+' : ''}${s.offc.toFixed(1)} °C`,
          () => set(`offc=${clamp(s.offc - 0.1, -15, 15)}`),
          () => set(`offc=${clamp(s.offc + 0.1, -15, 15)}`),
        ),
        step(
          'Probe offset · idle',
          'Correction added to the garage sensor the rest of the time.',
          `${s.offi > 0 ? '+' : ''}${s.offi.toFixed(1)} °C`,
          () => set(`offi=${clamp(s.offi - 0.1, -15, 15)}`),
          () => set(`offi=${clamp(s.offi + 0.1, -15, 15)}`),
        ),
        text(
          'Garage probe',
          s.sensor
            ? 'On-board BME280 — temperature, humidity and pressure.'
            : 'The on-board BME280 did not answer at boot; garage readings are unavailable.',
          s.sensor ? 'BME280 OK' : 'NOT FOUND',
        ),
        text(
          'Outside temperature',
          'Subscribed topic the yard reading arrives on. A reading older than 30 minutes counts as stale and auto holds.',
          info?.topic_out ?? '–',
        ),
        text(
          'Sample interval',
          'How often a reading is taken, logged to the card and pushed into the 24 h ring.',
          info ? `${info.sample_s / 60} min` : '–',
        ),
      ],
    },
    {
      title: 'NETWORK',
      blurb:
        'Where the controller publishes and how it is reached. Local logging on the card continues through any outage.',
      rows: [
        text('MQTT broker', 'Address the controller publishes state to and takes commands from.',
          info?.broker ?? '–'),
        text('Link', 'Access point joined at boot, and the current signal.',
          `${info?.ssid ?? '–'} · ${s.rssi} dBm`),
        text('Address', 'Where this page is served from.',
          `${s.ip || '–'}${info ? ` · ${info.host}.local` : ''}`),
        text('Command topic', 'Publish a speed 0–12 here to drive the fan from anywhere.',
          info?.topic_set ?? '–'),
      ],
    },
    {
      title: 'POWER',
      blurb:
        'The backup pack that keeps the controller alive and logging when the USB supply drops out. The fan itself runs from its own 12 V brick.',
      rows: [
        text('Cell voltage', 'Live pack voltage — the same series plotted in the BATTERY chart row.',
          s.batt ? `${s.batt.v.toFixed(3)} V` : 'no pack detected'),
        text(
          'Charge state',
          'Sticky verdict from the voltage slope, so trickle charging does not flap the temperature offset.',
          s.batt
            ? `${s.batt.chg ? 'charging' : 'discharging'}${
                s.batt.mvh !== null ? ` · ${s.batt.mvh >= 0 ? '+' : ''}${s.batt.mvh} mV/h` : ''
              }`
            : '–',
        ),
        text('Estimated runtime', 'Hours left at the current discharge slope. Blank while charging or flat.',
          s.batt?.eta_h != null ? `${s.batt.eta_h.toFixed(1)} h` : '–'),
      ],
    },
    {
      title: 'DEVICE',
      blurb:
        'Identity, storage and maintenance. A restart takes about eight seconds and does not clear logs.',
      rows: [
        text('Firmware', 'Running image and slot. See SLOT in the status bar for what “confirmed” means.',
          `${s.fw} · ${s.slot} · ${s.confirmed ? 'confirmed' : 'unconfirmed'}`),
        text('Device ID', 'Unique controller identifier, derived from the radio MAC.', info?.id ?? '–'),
        text(
          'Uptime',
          `Time since the last reboot.${
            s.prev_death && s.prev_death !== 'none' ? ` Previous boot ended in ${s.prev_death}.` : ''
          }`,
          `${hoursMinutes(s.uptime_s)} · boot ${s.boots}`,
        ),
        text(
          'Log storage',
          'microSD card holding the per-month CSV files behind the 7-day and 30-day ranges.',
          s.sd_total_mb
            ? storage(s.sd_used_mb, s.sd_total_mb, s.sd_free_mb)
            : s.sd_q
              ? 'quarantined'
              : 'not mounted',
        ),
        {
          kind: 'panel',
          label: 'Panel',
          hint: 'What the e-ink display on the device is showing right now. These are the same pixels the firmware sent to the glass, not a redrawing of them, so what you see here is what is on the wall. It repaints on the 5-minute sample cadence.',
        },
        {
          kind: 'actions',
          label: 'Maintenance',
          hint: 'Restart reboots into the same slot. "Delete card contents" unlinks every file but leaves the filesystem alone — use it to reclaim space, since Format only rewrites a card that will not mount. Both need the update token.',
          actions: [
            // 30 days off the card, not the 24 h RAM ring this used to serve.
            { text: 'Download CSV (30 d)', href: '/download.csv?days=30' },
            { text: 'Restart', run: d.restart },
            { text: 'Delete card contents', run: d.purgeCard, danger: true },
            { text: 'Format SD card', run: d.formatCard, danger: true },
          ],
        },
      ],
    },
    {
      title: 'UPDATE',
      blurb:
        'Firmware is written to the inactive A/B slot and confirmed only after the new image reaches the broker; an image that never checks in rolls back on its own.',
      rows: [
        {
          kind: 'update',
          label: 'Latest release',
          hint: 'Your browser asks GitHub — the controller itself never talks to the internet.',
          status: d.update,
          recheck: d.recheckUpdate,
        },
        {
          kind: 'ota',
          label: 'Upload firmware',
          hint: 'Pick the firmware.bin from a release (or from make build), enter the update token, and the board reboots into it.',
        },
      ],
    },
  ];
}

/** Render a spec into `host`, replacing whatever was there. */
export function render(host: HTMLElement, groups: Group[]): void {
  host.replaceChildren(...groups.map(renderGroup));
}

function renderGroup(g: Group): HTMLElement {
  const wrap = el('div', { className: 'grp' });
  const head = el('div');
  head.append(
    el('div', { className: 'gt', textContent: g.title }),
    el('div', { className: 'gb', textContent: g.blurb }),
  );
  const rows = el('div', { className: 'grows' });
  rows.append(...g.rows.map(renderRow));
  wrap.append(head, rows);
  return wrap;
}

function renderRow(r: Row): HTMLElement {
  const row = el('div', { className: 'grow' });
  const left = el('div', { className: 'rl' });
  left.append(
    el('div', { className: 'rlab', textContent: r.label }),
    el('div', { className: 'rh', textContent: r.hint }),
  );
  row.append(left, control(r));
  return row;
}

function control(r: Row): HTMLElement {
  switch (r.kind) {
    case 'step': {
      const box = el('div', { className: 'stp' });
      const dec = el('button', { textContent: '−', onclick: r.dec });
      dec.setAttribute('aria-label', `decrease ${r.label}`);
      const inc = el('button', { textContent: '+', onclick: r.inc });
      inc.setAttribute('aria-label', `increase ${r.label}`);
      box.append(dec, el('span', { textContent: r.value }), inc);
      return box;
    }
    case 'toggle': {
      const b = el('button', { className: `tgl${r.on ? ' on' : ''}`, onclick: r.toggle });
      b.setAttribute('role', 'switch');
      b.setAttribute('aria-checked', r.on ? 'true' : 'false');
      b.setAttribute('aria-label', r.label);
      b.append(el('i'));
      return b;
    }
    case 'text':
      return el('div', { className: 'rtx', textContent: r.value });
    case 'actions': {
      const box = el('div', { className: 'acts' });
      for (const a of r.actions) {
        if (a.href) {
          box.append(el('a', { textContent: a.text, href: a.href }));
        } else {
          const b = el('button', { textContent: a.text });
          if (a.danger) b.className = 'danger';
          if (a.run) b.onclick = a.run;
          box.append(b);
        }
      }
      return box;
    }
    case 'update':
      return updateControl(r.status, r.recheck);
    case 'ota':
      return otaControl();
    case 'panel':
      return panelControl();
  }
}

function updateControl(status: UpdateStatus | null, recheck: () => void): HTMLElement {
  const box = el('div', { className: 'upd' });
  const line = el('div', { className: 'updline' });

  const notes = (release: { html_url: string }) =>
    el('a', {
      className: 'updlink',
      textContent: 'release notes',
      href: release.html_url,
      target: '_blank',
      rel: 'noopener noreferrer',
    });

  if (status === null) {
    line.append(el('span', { className: 'updmuted', textContent: 'checking…' }));
  } else if (status.kind === 'available') {
    line.append(el('span', { className: 'updnew', textContent: `${status.latest} available` }));
    line.append(notes(status.release));
    // Downloads the image to the machine running the browser; installing it
    // is still the deliberate OTA upload below.
    line.append(el('a', {
      className: 'updlink',
      textContent: 'download .bin',
      href: status.asset.browser_download_url,
    }));
  } else if (status.kind === 'no-binary') {
    // Not shown as "available": there is nothing the operator can install.
    line.append(el('span', {
      className: 'updmuted',
      textContent: `${status.latest} tagged, but it published no .bin`,
    }));
    line.append(notes(status.release));
  } else if (status.kind === 'ahead') {
    // Deliberately NOT the green "up to date". The device being ahead of the
    // newest published release means the release channel is stale, which is
    // the opposite of an all-clear -- the deployed fan sat 22 versions ahead
    // of v1.14.0 while this row rendered a reassuring green tick.
    line.append(el('span', {
      className: 'updmuted',
      textContent: `running ${status.running}, newer than the latest release (${status.latest}) — unreleased build`,
    }));
  } else if (status.kind === 'up-to-date') {
    line.append(el('span', { className: 'updok', textContent: `up to date (${status.latest})` }));
  } else {
    line.append(el('span', { className: 'updmuted', textContent: `unknown — ${status.reason}` }));
  }

  line.append(el('button', { className: 'updbtn', textContent: 'Check again', onclick: recheck }));
  box.append(line);
  return box;
}

/**
 * The OTA row, built once and reused.
 *
 * Every other control is cheap to recreate; this one holds state the operator
 * typed (the token), a picked File, and the progress line for an upload that
 * is still running. Rebuilding it discarded all three -- the upload handler
 * captured #otamsg before awaiting and then wrote to a node that had since
 * been replaced, so a ~1 MB upload showed "uploading…" and then nothing,
 * forever, whichever way it ended.
 */
let otaRow: HTMLElement | null = null;

function otaControl(): HTMLElement {
  if (otaRow) return otaRow;
  const box = el('div');
  const row = el('div', { id: 'otarow' });
  const file = el('input', { className: 'fld', id: 'ota_f', type: 'file', accept: '.bin' });
  file.setAttribute('aria-label', 'firmware image file');
  const token = el('input', {
    className: 'fld',
    id: 'ota_t',
    type: 'password',
    placeholder: 'update token',
    autocomplete: 'off',
  });
  const go = el('button', { id: 'ota_go', textContent: 'Upload' });
  row.append(file, token, go);
  const msg = el('div', { id: 'otamsg' });
  box.append(row, msg);
  show(msg, true);
  otaRow = box;
  return box;
}


// --------------------------------------------------------------- the mirror

let panelRow: HTMLElement | null = null;

/**
 * The e-ink panel, as it is right now.
 *
 * A canvas rather than a re-drawn layout: the device sends the same two 1-bit
 * planes it clocked to the glass, so this cannot drift from the wall. See
 * panel.ts and the DisplayFrame contract in types.ts.
 *
 * The panel refreshes on the 5-minute sample cadence, so what you see here is
 * usually a few minutes old -- said out loud under the image, because a mirror
 * that looks stale and does not explain itself reads as a broken mirror.
 */
function panelControl(): HTMLElement {
  if (panelRow) return panelRow;
  const box = el('div', { className: 'pnl' });
  const cv = el('canvas', { id: 'pnlcv' }) as HTMLCanvasElement;
  const msg = el('div', { className: 'pnlmsg', id: 'pnlmsg', textContent: 'loading the panel…' });
  const btn = el('button', {
    className: 'updbtn',
    id: 'pnlrefresh',
    textContent: 'Refresh now',
    onclick: () => void forcePanelRefresh(cv, msg),
  });
  box.append(cv, msg, btn);
  panelRow = box;
  // The elements are captured, NOT looked up by id later: this function returns
  // the box for the caller to append, so at this point nothing here is in the
  // document yet and a getElementById would find nothing and quietly do
  // nothing. That is exactly how the first version of this shipped a canvas
  // that never fetched a frame.
  void loadPanel(cv, msg);
  return box;
}

async function loadPanel(cv: HTMLCanvasElement, msg: HTMLElement): Promise<void> {
  try {
    const frame = await api.getDisplay();
    if (!frame.ready) {
      msg.textContent = ageText(frame);
      return;
    }
    paintFrame(cv, frame);
    // Name the panel kind: on a mono part the accents render grey here, and
    // without saying so the mirror looks like it lost its colour.
    msg.textContent = frame.tricolor
      ? `${ageText(frame)} · tricolor panel`
      : `${ageText(frame)} · mono panel (the red plane is not shown, because the glass cannot)`;
  } catch (err) {
    msg.textContent = `could not read the panel: ${err instanceof Error ? err.message : String(err)}`;
  }
}

async function forcePanelRefresh(cv: HTMLCanvasElement, msg: HTMLElement): Promise<void> {
  msg.textContent = 'refreshing the panel…';
  try {
    await api.refreshDisplay();
    // The panel blocks the device for seconds while it clocks the waveform, so
    // give it a beat before asking for the new frame.
    await new Promise((r) => setTimeout(r, 4000));
  } catch (err) {
    // Reported, not swallowed. Falling through to loadPanel would paint the
    // PREVIOUS frame under a normal "refreshed Nm ago" line, so a refusal or a
    // dead controller would read as a slightly stale panel. The device answers
    // 429 with retry_in_s when a repaint comes too soon, and the operator
    // should see that rather than a reassuring timestamp.
    msg.textContent = `could not refresh the panel: ${err instanceof Error ? err.message : String(err)}`;
    return;
  }
  await loadPanel(cv, msg);
}
