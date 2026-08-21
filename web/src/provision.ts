// The connection form in the settings drawer: WiFi, MQTT and the weather
// coordinates, stored on the board (creds.h, NVS) and changed through
// POST /api/provision. This is what lets a published release image -- built
// without anyone's passwords -- run on a provisioned board, which is what lets
// "update the fan" stop meaning "find a laptop".
//
// The diff is the design: only fields the operator actually changed travel,
// and the password boxes are write-only -- blank means "leave it", since the
// device never sends a password down for the form to pre-fill.

import * as api from './api.js';
import { tokenFromFieldOrPrompt } from './ota.js';
import type { DeviceInfo } from './types.js';

export interface ProvisionForm {
  ssid: string;
  pass: string;
  mqtt_host: string;
  mqtt_port: string;
  mqtt_user: string;
  mqtt_pass: string;
  lat: string;
  lon: string;
}

/** "host:port" -> parts; a malformed broker string reads as empty. */
export function splitBroker(broker: string): { host: string; port: string } {
  const i = broker.lastIndexOf(':');
  if (i <= 0) return { host: broker, port: '' };
  return { host: broker.slice(0, i), port: broker.slice(i + 1) };
}

/** What the form shows before the operator touches it. */
export function formFromInfo(info: DeviceInfo | null): ProvisionForm {
  const b = splitBroker(info?.broker ?? '');
  return {
    ssid: info?.ssid ?? '',
    pass: '',
    mqtt_host: b.host,
    mqtt_port: b.port,
    mqtt_user: info?.mqtt_user ?? '',
    mqtt_pass: '',
    lat: info?.lat ?? '',
    lon: info?.lon ?? '',
  };
}

/**
 * The query string for /api/provision: changed fields only, passwords only
 * when typed. Empty when nothing would change, so the caller can refuse to
 * reboot the board for no reason.
 */
export function provisionQuery(info: DeviceInfo | null, form: ProvisionForm): string {
  const base = formFromInfo(info);
  const out: string[] = [];
  const add = (k: keyof ProvisionForm) => out.push(`${k}=${encodeURIComponent(form[k].trim())}`);
  for (const k of ['ssid', 'mqtt_host', 'mqtt_port', 'mqtt_user', 'lat', 'lon'] as const) {
    if (form[k].trim() !== base[k].trim()) add(k);
  }
  for (const k of ['pass', 'mqtt_pass'] as const) {
    if (form[k].length) add(k);
  }
  return out.join('&');
}

/** Local sanity before the round trip; the firmware re-checks all of it. */
export function validate(form: ProvisionForm): string | null {
  if (form.ssid.trim().length === 0) return 'the WiFi name cannot be blank';
  if (form.ssid.trim().length > 32) return 'WiFi names are at most 32 characters';
  if (form.pass.length > 63) return 'WiFi passwords are at most 63 characters';
  if (form.mqtt_port.trim().length) {
    const p = Number(form.mqtt_port);
    if (!Number.isInteger(p) || p < 1 || p > 65535) return 'the MQTT port must be 1–65535';
  }
  for (const k of ['lat', 'lon'] as const) {
    const v = form[k].trim();
    if (v.length && (!Number.isFinite(Number(v)) || v.length > 16))
      return `${k === 'lat' ? 'latitude' : 'longitude'} must be a decimal number`;
  }
  return null;
}

// --------------------------------------------------------------- the control

let box: HTMLElement | null = null;
const inputs: Partial<Record<keyof ProvisionForm, HTMLInputElement>> = {};

function field(
  key: keyof ProvisionForm,
  placeholder: string,
  type: 'text' | 'password' | 'number' = 'text',
): HTMLInputElement {
  const i = document.createElement('input');
  i.className = 'fld';
  i.id = `prov_${key}`;
  i.type = type;
  i.placeholder = placeholder;
  i.autocomplete = 'off';
  i.setAttribute('aria-label', placeholder);
  inputs[key] = i;
  return i;
}

function read(): ProvisionForm {
  const v = (k: keyof ProvisionForm) => inputs[k]?.value ?? '';
  return {
    ssid: v('ssid'),
    pass: v('pass'),
    mqtt_host: v('mqtt_host'),
    mqtt_port: v('mqtt_port'),
    mqtt_user: v('mqtt_user'),
    mqtt_pass: v('mqtt_pass'),
    lat: v('lat'),
    lon: v('lon'),
  };
}

/** Pre-fill from the device without clobbering what the operator is typing. */
export function fillFrom(info: DeviceInfo | null): void {
  const f = formFromInfo(info);
  for (const k of Object.keys(f) as (keyof ProvisionForm)[]) {
    const i = inputs[k];
    if (i && !i.dataset['touched'] && (k !== 'pass' && k !== 'mqtt_pass')) i.value = f[k];
  }
}

async function save(info: () => DeviceInfo | null, msg: HTMLElement): Promise<void> {
  const form = read();
  const why = validate(form);
  if (why) {
    msg.textContent = why;
    return;
  }
  const q = provisionQuery(info(), form);
  if (!q) {
    msg.textContent = 'nothing changed';
    return;
  }
  const token = tokenFromFieldOrPrompt('Update token to change the connection settings:');
  if (!token) return;
  if (
    !window.confirm(
      'Save these connection settings? The controller reboots to apply them; if the WiFi details are wrong it will be unreachable until it is re-flashed over USB.',
    )
  )
    return;
  msg.textContent = 'saving…';
  try {
    const res = await api.provision(q, token);
    msg.textContent = res.ok
      ? 'saved — the controller is rebooting onto the new settings'
      : `refused: ${res.error ?? 'unknown error'}`;
  } catch (err) {
    msg.textContent = `request failed: ${err instanceof Error ? err.message : String(err)}`;
  }
}

export function provisionControl(info: () => DeviceInfo | null): HTMLElement {
  if (box) {
    fillFrom(info());
    return box;
  }
  box = document.createElement('div');
  box.className = 'prov';
  const grid = document.createElement('div');
  grid.className = 'provgrid';
  grid.append(
    field('ssid', 'WiFi name'),
    field('pass', 'WiFi password (unchanged if blank)', 'password'),
    field('mqtt_host', 'MQTT broker host'),
    field('mqtt_port', 'port', 'number'),
    field('mqtt_user', 'MQTT user'),
    field('mqtt_pass', 'MQTT password (unchanged if blank)', 'password'),
    field('lat', 'latitude'),
    field('lon', 'longitude'),
  );
  for (const i of Object.values(inputs)) i.oninput = () => (i.dataset['touched'] = '1');
  const go = document.createElement('button');
  go.id = 'prov_go';
  go.textContent = 'Save & reboot';
  const msg = document.createElement('div');
  msg.id = 'provmsg';
  go.onclick = () => void save(info, msg);
  box.append(grid, go, msg);
  fillFrom(info());
  return box;
}
