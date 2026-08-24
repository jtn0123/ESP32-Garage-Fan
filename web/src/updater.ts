// One-click install: the newest published release, from the update channel
// to the controller's flash, without a laptop, a build or a file picker.
//
// Why the browser is in the middle: the ESP32-S2 cannot fetch from GitHub
// itself (no TLS -- see net/weather.h for the measured reason), and GitHub
// release assets cannot be fetched by a browser from another origin (no CORS
// headers on the asset download). release.yml therefore publishes the same
// image and checksum to GitHub Pages, which serves with
// Access-Control-Allow-Origin: *. This module downloads from there, verifies
// the SHA-256 against the published sidecar, hands the bytes to /update, and
// then watches the board come back -- reporting CONFIRMED only once the new
// image has reached the broker, because until then it can still roll back.

import { parseChecksum, sha256Hex, type UpdateStatus } from './update.js';
import type { DeviceState } from './types.js';

/** The firmware's compiled default (config.h FAN_OTA_TOKEN). Public by the
 * operator's choice; used when the token field is blank so the common case
 * is genuinely one click. A refused token falls back to asking. */
export const DEFAULT_TOKEN = 'iliving-ota'; // gitleaks:allow -- config.h's committed public default

/** Where release.yml publishes the channel for `owner/name`. */
export function pagesUrl(repo: string, file: string): string {
  const [owner, name] = repo.split('/');
  return `https://${owner}.github.io/${name}/firmware/${file}`;
}

export type RebootVerdict = 'waiting' | 'confirmed' | 'unconfirmed' | 'rolled-back';

/**
 * Read one post-upload state sample. `before` is the state at upload time:
 * the boot counter is what tells "the old image is still answering" from
 * "it rebooted" -- fw alone cannot, because the old image reports the old fw
 * right up to the reset.
 */
export function judgeReboot(
  before: { fw: string; boots: number },
  target: string,
  s: Pick<DeviceState, 'fw' | 'boots' | 'confirmed'>,
): RebootVerdict {
  if (s.boots <= before.boots) return 'waiting';
  if (s.fw === target) return s.confirmed ? 'confirmed' : 'unconfirmed';
  return 'rolled-back';
}

export interface InstallDeps {
  fetch: (url: string) => Promise<Response>;
  upload: (file: File, token: string) => Promise<string>;
  getState: () => Promise<DeviceState>;
  state: () => DeviceState | null;
  askToken: () => string | null;
  say: (text: string) => void;
  sleep: (ms: number) => Promise<void>;
  /** Total time to wait for confirmation; default 3 minutes. */
  confirmMs?: number;
}

/** Run the whole install. Resolves to the final line for the operator. */
export async function installUpdate(
  st: Extract<UpdateStatus, { kind: 'available' }>,
  repo: string,
  token: string,
  d: InstallDeps,
): Promise<string> {
  const name = st.asset.name;
  const url = pagesUrl(repo, name);
  d.say(`downloading ${st.latest} (${(st.asset.size / 1048576).toFixed(1)} MB)…`);
  let res: Response;
  try {
    res = await d.fetch(url);
  } catch (err) {
    return `could not reach the update channel (${err instanceof Error ? err.message : String(err)}).`;
  }
  if (!res.ok) {
    return `${st.latest} is not on the update channel yet — publishing takes about a minute after a release. Try again shortly, or upload the file by hand below.`;
  }
  const blob = await res.blob();
  d.say('checking the image…');
  const expected = await d
    .fetch(`${url}.sha256`)
    .then((r) => (r.ok ? r.text() : null))
    .then((t) => (t ? parseChecksum(t) : null))
    .catch(() => null);
  if (!expected) return `ABORTED: no published checksum for ${st.latest}. Nothing was sent.`;
  if ((await sha256Hex(blob)) !== expected) {
    return 'ABORTED: the downloaded image does not match its published checksum. Nothing was sent to the controller.';
  }
  const before = d.state();
  if (!before) return 'the controller has not answered yet; try again in a moment.';
  const snapshot = { fw: before.fw, boots: before.boots };
  d.say(`verified — uploading ${st.latest}…`);
  const file = new File([blob], name);
  let body = await d.upload(file, token);
  if (/bad token/.test(body)) {
    const again = d.askToken();
    if (!again) return 'the controller refused the update token — nothing was installed.';
    body = await d.upload(file, again);
  }
  if (/"error"/.test(body)) return `upload failed: ${body}`;
  d.say(`uploaded — the controller is rebooting into ${st.latest}…`);
  const deadline = Date.now() + (d.confirmMs ?? 180_000);
  while (Date.now() < deadline) {
    await d.sleep(3000);
    let s: DeviceState;
    try {
      s = await d.getState();
    } catch {
      continue; // mid-reboot: nobody answers, which is expected
    }
    const v = judgeReboot(snapshot, st.latest.replace(/^v/, ''), s);
    if (v === 'confirmed') return `updated to ${st.latest} and confirmed ✓`;
    if (v === 'unconfirmed') d.say(`${st.latest} is running, waiting for it to reach the broker…`);
    if (v === 'rolled-back') {
      return `the controller rebooted but came back on ${s.fw} — the new image was rolled back before it reached the broker. Check the flight recorder.`;
    }
  }
  return `no confirmation after ${Math.round((d.confirmMs ?? 180_000) / 60000)} minutes — check the FW line; an image that never confirms rolls back on its own.`;
}
