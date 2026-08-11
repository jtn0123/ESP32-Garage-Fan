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
  | { kind: 'available'; latest: string; release: Release; asset: ReleaseAsset }
  /**
   * The device is running something NEWER than anything published.
   *
   * Distinguished from 'up-to-date' because it means the opposite thing about
   * the release channel. The deployed fan ran 1.14.22 while the newest tag was
   * v1.14.0 -- 22 versions of stability work that existed only on main -- and
   * the console rendered a green "up to date (v1.14.0)", which reads as an
   * all-clear. A device ahead of the feed is an unreleased-work signal.
   */
  | { kind: 'ahead'; latest: string; running: string }
  /** A newer tag exists but published no OTA image, so it cannot be installed. */
  | { kind: 'no-binary'; latest: string; release: Release }
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
    const asset = pickFirmwareAsset(latest.assets);
    // A release with no OTA image is not something the operator can act on.
    // It used to be shown as a green "available" badge with a muted "no binary
    // attached yet" beside it -- the same prominence as an installable one.
    if (!asset) return { kind: 'no-binary', latest: latest.tag_name, release: latest };
    return { kind: 'available', latest: latest.tag_name, release: latest, asset };
  }
  if (verdict === 'older') {
    return { kind: 'ahead', latest: latest.tag_name, running: runningVersion };
  }
  return { kind: 'up-to-date', latest: latest.tag_name };
}

/**
 * SHA-256 of a picked file, lowercase hex.
 *
 * The release workflow publishes a `.sha256` beside every image and calls it
 * "the only integrity check between GitHub and the flash" -- there is no
 * secure boot and /update accepts whatever bytes it is handed. That sidecar
 * was published, unit-tested, and then never actually consulted by anything.
 * This is the missing half.
 */
export async function sha256Hex(file: Blob): Promise<string> {
  const digest = await crypto.subtle.digest('SHA-256', await file.arrayBuffer());
  return Array.from(new Uint8Array(digest))
    .map((b) => b.toString(16).padStart(2, '0'))
    .join('');
}

/** Pull the expected digest out of a `sha256sum` sidecar ("<hex>  <name>"). */
export function parseChecksum(body: string): string | null {
  const m = /^([0-9a-f]{64})\b/i.exec(body.trim());
  return m ? m[1]!.toLowerCase() : null;
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
