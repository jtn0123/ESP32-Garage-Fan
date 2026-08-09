// Tier 1 update check: the browser asks GitHub, the device stays out of it.
//
// The alternative was linking TLS into the firmware so the ESP32-S2 could call
// api.github.com itself. That costs roughly 60-100 KB of flash against ~265 KB
// free, 40-50 KB of heap during the handshake, and a root CA that expires --
// all to answer a question the browser rendering this page can already answer
// for free. The device is never told the outcome; it only ever receives a
// firmware image through the OTA upload it already had.
//
// Everything here is pure apart from `fetchLatestRelease`, which takes its
// fetch implementation as a parameter so the tests never touch the network.

import { isUpgrade, versionCode, type Comparison } from './version.js';

export interface ReleaseAsset {
  name: string;
  browser_download_url: string;
  size: number;
}

/** The subset of GitHub's release object we rely on. */
export interface Release {
  tag_name: string;
  name: string | null;
  html_url: string;
  body: string | null;
  draft: boolean;
  prerelease: boolean;
  assets: ReleaseAsset[];
}

export type UpdateStatus =
  | { kind: 'up-to-date'; latest: string }
  | { kind: 'available'; latest: string; release: Release; asset: ReleaseAsset | null }
  | { kind: 'unknown'; reason: string };

/**
 * Pick the OTA image out of a release's assets.
 *
 * release.yml names it `garage-fan-v<version>.bin`. The `.factory.bin` in the
 * same release is a full-flash image including the bootloader -- handing that
 * to /update would brick the board, so it is excluded explicitly rather than
 * by hoping the ordering works out.
 */
export function pickFirmwareAsset(assets: readonly ReleaseAsset[]): ReleaseAsset | null {
  for (const a of assets) {
    const n = a.name.toLowerCase();
    if (n.endsWith('.bin') && !n.endsWith('.factory.bin')) return a;
  }
  return null;
}

/** The `.sha256` sidecar for an asset, if the release published one. */
export function pickChecksumAsset(
  assets: readonly ReleaseAsset[],
  firmware: ReleaseAsset,
): ReleaseAsset | null {
  const want = `${firmware.name.toLowerCase()}.sha256`;
  for (const a of assets) if (a.name.toLowerCase() === want) return a;
  return null;
}

/**
 * Decide what to show, given the running version and whatever GitHub returned.
 *
 * Separated from the fetch so every branch is unit-testable without a network
 * or a DOM.
 */
export function evaluate(runningVersion: string, releases: readonly Release[]): UpdateStatus {
  // GitHub orders by creation time, and a backported hotfix (say v1.13.1
  // tagged after v2.0.0) would sit first. Compare parsed versions instead and
  // offer the highest stable one. Drafts and pre-releases are skipped: a
  // garage fan should not be offered a release candidate.
  const usable = releases.filter((r) => !r.draft && !r.prerelease);
  if (!usable.length) return { kind: 'unknown', reason: 'no published releases yet' };
  let latest = usable[0]!;
  let latestCode = versionCode(latest.tag_name);
  for (const r of usable.slice(1)) {
    const code = versionCode(r.tag_name);
    if (code !== null && (latestCode === null || code > latestCode)) {
      latest = r;
      latestCode = code;
    }
  }

  const verdict: Comparison = isUpgrade(runningVersion, latest.tag_name);
  if (verdict === 'unknown') {
    return {
      kind: 'unknown',
      reason: `cannot compare "${runningVersion}" with "${latest.tag_name}"`,
    };
  }
  if (verdict === 'newer') {
    return {
      kind: 'available',
      latest: latest.tag_name,
      release: latest,
      asset: pickFirmwareAsset(latest.assets),
    };
  }
  return { kind: 'up-to-date', latest: latest.tag_name };
}

export type FetchLike = (url: string, init?: RequestInit) => Promise<Response>;

/**
 * Ask GitHub for recent releases for `repo` ("owner/name").
 *
 * Unauthenticated, so we are sharing a 60 requests/hour/IP budget with every
 * other tab on this network -- hence one call per page load and no polling.
 * `/releases` rather than `/releases/latest` so a tag that carries no binary
 * yet does not read as "no releases".
 */
export async function fetchReleases(
  repo: string,
  doFetch: FetchLike = fetch,
  perPage = 5,
): Promise<Release[]> {
  const url = `https://api.github.com/repos/${repo}/releases?per_page=${perPage}`;
  const res = await doFetch(url, { headers: { Accept: 'application/vnd.github+json' } });
  if (!res.ok) throw new Error(`github returned ${res.status}`);
  const body: unknown = await res.json();
  if (!Array.isArray(body)) throw new Error('unexpected releases payload');
  return body as Release[];
}

/** One-shot check. Never throws: a failure becomes an `unknown` status. */
export async function checkForUpdate(
  repo: string,
  runningVersion: string,
  doFetch: FetchLike = fetch,
): Promise<UpdateStatus> {
  try {
    return evaluate(runningVersion, await fetchReleases(repo, doFetch));
  } catch (err) {
    return { kind: 'unknown', reason: err instanceof Error ? err.message : String(err) };
  }
}
