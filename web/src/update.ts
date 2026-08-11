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
  const bytes = new Uint8Array(await file.arrayBuffer());
  // WebCrypto is only exposed in a SECURE CONTEXT. This console is served over
  // plain HTTP from the device itself, so `crypto.subtle` is undefined in
  // exactly the deployment that matters -- the integrity check would have been
  // dead code on every real page load while looking implemented. Use it when
  // it exists (localhost, or a future TLS front end) and fall back to the
  // local implementation otherwise.
  if (typeof crypto !== 'undefined' && crypto.subtle) {
    const digest = await crypto.subtle.digest('SHA-256', bytes);
    return hex(new Uint8Array(digest));
  }
  return hex(sha256(bytes));
}

function hex(b: Uint8Array): string {
  let s = '';
  for (const v of b) s += v.toString(16).padStart(2, '0');
  return s;
}

/** FIPS 180-4 round constants (first 32 bits of the cube roots of primes 2..311). */
const K = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);

/**
 * SHA-256 over `data`. Straight FIPS 180-4; exported for its own test vectors.
 *
 * Exists because the device serves the console over plain HTTP, where
 * `crypto.subtle` is absent. ~1 MB of firmware hashes in well under a second,
 * which is nothing next to the upload that follows.
 */
export function sha256(data: Uint8Array): Uint8Array {
  const h = new Uint32Array([
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
  ]);
  // Message + 0x80 + zero padding + 64-bit big-endian bit length.
  const bitLen = data.length * 8;
  // Smallest multiple of 64 that holds the message + 0x80 + the 8-byte length.
  // It must be the SMALLEST: an extra all-zero block is still a valid-looking
  // buffer but hashes to something else entirely, and it only happens when
  // len % 64 == 55, which the published FIPS vectors do not cover. Caught by
  // differential-testing this against Node's crypto, not by the vectors.
  const withPad = new Uint8Array(((data.length + 9 + 63) >> 6) << 6);
  withPad.set(data);
  withPad[data.length] = 0x80;
  const dv = new DataView(withPad.buffer);
  // Length is 64-bit; JS bit ops are 32-bit, so write the halves separately.
  dv.setUint32(withPad.length - 8, Math.floor(bitLen / 0x100000000), false);
  dv.setUint32(withPad.length - 4, bitLen >>> 0, false);

  const w = new Uint32Array(64);
  const rotr = (x: number, n: number): number => (x >>> n) | (x << (32 - n));

  for (let off = 0; off < withPad.length; off += 64) {
    for (let i = 0; i < 16; i++) w[i] = dv.getUint32(off + i * 4, false);
    for (let i = 16; i < 64; i++) {
      const a = w[i - 15]!;
      const b = w[i - 2]!;
      const s0 = rotr(a, 7) ^ rotr(a, 18) ^ (a >>> 3);
      const s1 = rotr(b, 17) ^ rotr(b, 19) ^ (b >>> 10);
      w[i] = (w[i - 16]! + s0 + w[i - 7]! + s1) >>> 0;
    }
    let [a, b, c, d, e, f, g, hh] = [h[0]!, h[1]!, h[2]!, h[3]!, h[4]!, h[5]!, h[6]!, h[7]!];
    for (let i = 0; i < 64; i++) {
      const S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const ch = (e & f) ^ (~e & g);
      const t1 = (hh + S1 + ch + K[i]! + w[i]!) >>> 0;
      const S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const maj = (a & b) ^ (a & c) ^ (b & c);
      const t2 = (S0 + maj) >>> 0;
      hh = g;
      g = f;
      f = e;
      e = (d + t1) >>> 0;
      d = c;
      c = b;
      b = a;
      a = (t1 + t2) >>> 0;
    }
    h[0] = (h[0]! + a) >>> 0;
    h[1] = (h[1]! + b) >>> 0;
    h[2] = (h[2]! + c) >>> 0;
    h[3] = (h[3]! + d) >>> 0;
    h[4] = (h[4]! + e) >>> 0;
    h[5] = (h[5]! + f) >>> 0;
    h[6] = (h[6]! + g) >>> 0;
    h[7] = (h[7]! + hh) >>> 0;
  }

  const out = new Uint8Array(32);
  const odv = new DataView(out.buffer);
  for (let i = 0; i < 8; i++) odv.setUint32(i * 4, h[i]!, false);
  return out;
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
