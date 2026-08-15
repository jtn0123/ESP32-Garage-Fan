// Typed wrappers over the firmware's HTTP surface.
//
// Every call funnels through json() so a 404 or a truncated body becomes a
// thrown Error the caller can handle, rather than `undefined` propagating into
// the render and showing up as NaN three functions later.

import type {
  Boots,
  DeviceInfo,
  DeviceState,
  DisplayFrame,
  History,
  Sensors,
  Stats,
} from './types.js';

async function json<T>(url: string, method: 'GET' | 'POST' = 'GET'): Promise<T> {
  // Timeout so a device that drops off the network mid-request fails the
  // call (and the poll retries) instead of holding a pending fetch forever.
  const res = await fetch(url, { method, signal: AbortSignal.timeout(10_000) });
  if (!res.ok) throw new Error(`${url} -> ${res.status}`);
  return (await res.json()) as T;
}

export const getState = (): Promise<DeviceState> => json<DeviceState>('/api/state');
export const getDevice = (): Promise<DeviceInfo> => json<DeviceInfo>('/api/device');
export const getStats = (): Promise<Stats> => json<Stats>('/api/stats');
export const getSensors = (): Promise<Sensors> => json<Sensors>('/api/sensors');
/** The e-ink panel's last frame, for the mirror in the settings drawer. */
export const getDisplay = (): Promise<DisplayFrame> => json<DisplayFrame>('/api/display');
/** Repaint the panel now instead of waiting out the 5-minute cadence. */
export const refreshDisplay = async (): Promise<Response> => {
  const r = await fetch('/api/display/refresh', { method: 'POST' });
  if (r.status === 429) {
    // The device refuses a repaint that comes too soon: each one parks its loop
    // for seconds. Say how long rather than reporting a bare status code.
    const body = (await r.json().catch(() => ({}))) as { retry_in_s?: number };
    throw new Error(`too soon — the panel can repaint again in ${body.retry_in_s ?? '?'}s`);
  }
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  return r;
};
export const getHistory = (days: number): Promise<History> =>
  json<History>(`/api/history?days=${days}`);

/**
 * Restart marks for the same window. Deliberately a SEPARATE request: the
 * charts must still render when this fails (an older firmware has no such
 * route), so the caller treats an empty list and a failed fetch alike.
 */
export const getBoots = (days: number): Promise<Boots> =>
  json<Boots>(`/api/boots?days=${days}`);

/**
 * Every command endpoint answers with the new state, so the UI never guesses.
 *
 * POST, not GET: these change the device, and the firmware now registers them
 * POST-only. As GETs they fired from any `<img src>` on any page the operator
 * had open, and from link prefetchers -- the same reasoning that already made
 * /api/restart and /api/sdformat POST-only.
 */
export const setSpeed = (speed: number): Promise<DeviceState> =>
  json<DeviceState>(`/api/set?speed=${speed}`, 'POST');

export const setConfig = (query: string): Promise<DeviceState> =>
  json<DeviceState>(`/api/config?${query}`, 'POST');

/** Token-guarded maintenance. Returns the raw body so the caller can show it. */
async function guarded(path: string, token: string): Promise<string> {
  // POST to match the firmware: these routes reboot the board or erase the
  // card, and are registered POST-only so GET prefetchers can never fire them.
  const res = await fetch(`${path}?token=${encodeURIComponent(token)}`, { method: 'POST' });
  return res.text();
}

export const restart = (token: string): Promise<string> => guarded('/api/restart', token);
export const formatCard = (token: string): Promise<string> => guarded('/api/sdformat', token);

/**
 * Delete the card's contents without reformatting it.
 *
 * Distinct from formatCard because format cannot reclaim space on a healthy
 * card: SD.begin's format_if_empty only rewrites a card that fails to MOUNT,
 * so a 99%-full 28 GB card is simply remounted intact.
 */
export const purgeCard = (token: string): Promise<string> => guarded('/api/sdpurge', token);

export async function uploadFirmware(file: File, token: string): Promise<string> {
  const body = new FormData();
  body.append('firmware', file);
  const res = await fetch(`/update?token=${encodeURIComponent(token)}`, { method: 'POST', body });
  return res.text();
}

/**
 * Live state over server-sent events on port 8081.
 *
 * Separate port because the firmware serves SSE from its own WiFiServer rather
 * than the WebServer -- see sse_accept(). Failure is silent on purpose: the
 * 15 s poll is the fallback and already covers it.
 */
export function subscribe(onState: (s: DeviceState) => void): void {
  try {
    const es = new EventSource(`http://${location.hostname}:8081/`);
    es.onmessage = (ev) => {
      try {
        onState(JSON.parse(ev.data as string) as DeviceState);
      } catch {
        /* a torn frame is not worth a console error every few seconds */
      }
    };
  } catch {
    /* EventSource unavailable; polling carries the page */
  }
}
