// The maintenance half of the settings drawer: the destructive card and
// controller operations, and the firmware upload with its checksum gate.
//
// Split out of app.ts at the 500-line ceiling. It is a clean seam: everything
// here is a one-shot user-confirmed action that talks to the device and
// reports through window dialogs or the OTA status line -- none of it touches
// the view-model or the paint cycle.

import * as api from './api.js';
import { view } from './state.js';
import { parseChecksum, sha256Hex } from './update.js';

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

export async function maintenance(kind: keyof typeof MAINTENANCE): Promise<void> {
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

/**
 * Hash the picked image and, when a published sum exists, compare against it.
 *
 * There is no secure boot and /update accepts whatever bytes it is handed, so
 * the release's `.sha256` sidecar is the only thing standing between GitHub
 * and the flash -- and nothing consulted it until now. Two rules:
 *
 *  - Fail closed. "We could not hash it" is not a reason to write it anyway.
 *  - Say whether anything was actually COMPARED. Printing a digest on its own
 *    reads like a passed check, and in the common cases (a locally built
 *    image, or a check that came back ahead/no-binary/up-to-date) there is no
 *    published sum and nothing was verified at all.
 */
async function verifyImage(file: File): Promise<{ ok: boolean; note: string }> {
  let digest: string;
  try {
    digest = await sha256Hex(file);
  } catch (err) {
    const why = err instanceof Error ? err.message : String(err);
    return { ok: false, note: `ABORTED: could not hash the image (${why}).` };
  }
  const short = `${digest.slice(0, 16)}…`;
  const upd = view.update;
  if (upd?.kind !== 'available') {
    return { ok: true, note: `NOT VERIFIED — no published checksum to compare; sha256 ${short}` };
  }
  const { asset, latest } = upd;
  const expected = await fetch(`${asset.browser_download_url}.sha256`)
    .then((r) => (r.ok ? r.text() : null))
    .then((t) => (t ? parseChecksum(t) : null))
    .catch(() => null);
  if (!expected) {
    return {
      ok: true,
      note: `NOT VERIFIED — could not fetch ${latest}'s published sum; sha256 ${short}`,
    };
  }
  if (expected !== digest) {
    return {
      ok: false,
      note: `ABORTED: this file's SHA-256 does not match ${latest}. Do not flash it.`,
    };
  }
  return { ok: true, note: `verified against ${latest} (sha256 ${short})` };
}

export async function uploadFirmware(): Promise<void> {
  const file = (document.getElementById('ota_f') as HTMLInputElement | null)?.files?.[0];
  const token = (document.getElementById('ota_t') as HTMLInputElement | null)?.value ?? '';
  const msg = document.getElementById('otamsg');
  if (!msg) return;
  if (!file) {
    msg.textContent = 'pick firmware.bin first';
    return;
  }
  msg.textContent = 'checking image…';
  const check = await verifyImage(file);
  msg.textContent = check.note;
  if (!check.ok) return;

  const size = `${(file.size / 1024).toFixed(0)} KB`;
  const prefix = msg.textContent ? `${msg.textContent} — ` : '';
  msg.textContent = `${prefix}uploading ${size}…`;
  try {
    const body = await api.uploadFirmware(file, token);
    // The endpoint answers JSON for both outcomes; surface the failure as one.
    msg.textContent = /"error"/.test(body) ? `upload failed: ${body}` : body;
  } catch (err) {
    msg.textContent = `upload failed: ${err instanceof Error ? err.message : String(err)}`;
  }
}
