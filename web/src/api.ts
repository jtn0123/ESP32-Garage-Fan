// Typed wrappers over the firmware's HTTP surface.
//
// Every call funnels through json() so a 404 or a truncated body becomes a
// thrown Error the caller can handle, rather than `undefined` propagating into
// the render and showing up as NaN three functions later.

import type { DeviceInfo, DeviceState, History, Sensors, Stats } from './types.js';

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
export const getHistory = (days: number): Promise<History> =>
  json<History>(`/api/history?days=${days}`);

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
