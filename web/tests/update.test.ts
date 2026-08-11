import { describe, expect, it, vi } from 'vitest';
import {
  checkForUpdate,
  evaluate,
  pickChecksumAsset,
  pickFirmwareAsset,
  sha256,
  type FetchLike,
  type Release,
  type ReleaseAsset,
} from '../src/update.js';

const asset = (name: string): ReleaseAsset => ({
  name,
  browser_download_url: `https://example.invalid/${name}`,
  size: 1024,
});

const release = (tag: string, over: Partial<Release> = {}): Release => ({
  tag_name: tag,
  name: `Release ${tag}`,
  html_url: `https://github.com/jtn0123/ESP32-Garage-Fan/releases/tag/${tag}`,
  body: 'notes',
  draft: false,
  prerelease: false,
  assets: [asset(`garage-fan-${tag}.bin`), asset(`garage-fan-${tag}.bin.sha256`)],
  ...over,
});

describe('pickFirmwareAsset', () => {
  it('picks the OTA image', () => {
    expect(pickFirmwareAsset(release('v1.14.0').assets)?.name).toBe('garage-fan-v1.14.0.bin');
  });

  it('never picks the factory image', () => {
    // .factory.bin includes the bootloader and partition table; POSTing it to
    // /update would write it at the app offset and brick the board.
    const assets = [asset('garage-fan-v1.14.0.factory.bin')];
    expect(pickFirmwareAsset(assets)).toBeNull();
  });

  it('prefers the OTA image even when the factory image comes first', () => {
    const assets = [asset('garage-fan-v1.14.0.factory.bin'), asset('garage-fan-v1.14.0.bin')];
    expect(pickFirmwareAsset(assets)?.name).toBe('garage-fan-v1.14.0.bin');
  });

  it('returns null when a release carries no binary', () => {
    expect(pickFirmwareAsset([asset('notes.txt')])).toBeNull();
  });
});

describe('pickChecksumAsset', () => {
  it('finds the sidecar for the chosen image', () => {
    const r = release('v1.14.0');
    const fw = pickFirmwareAsset(r.assets)!;
    expect(pickChecksumAsset(r.assets, fw)?.name).toBe('garage-fan-v1.14.0.bin.sha256');
  });

  it('returns null when no checksum was published', () => {
    const fw = asset('garage-fan-v1.14.0.bin');
    expect(pickChecksumAsset([fw], fw)).toBeNull();
  });
});

describe('evaluate', () => {
  it('offers a newer release', () => {
    const got = evaluate('1.13.0', [release('v1.14.0')]);
    expect(got.kind).toBe('available');
    if (got.kind === 'available') {
      expect(got.latest).toBe('v1.14.0');
      expect(got.asset?.name).toBe('garage-fan-v1.14.0.bin');
    }
  });

  it('reports up to date on an exact match', () => {
    expect(evaluate('1.13.0', [release('v1.13.0')]).kind).toBe('up-to-date');
  });

  it('reports "ahead" -- not "up to date" -- when the device leads the feed', () => {
    // These mean opposite things about the release channel. The deployed fan
    // ran 1.14.22 against a newest tag of v1.14.0 and the console showed a
    // green all-clear for a channel that had been dead for 22 versions.
    const got = evaluate('1.14.0', [release('v1.13.0')]);
    expect(got.kind).toBe('ahead');
    if (got.kind === 'ahead') {
      expect(got.running).toBe('1.14.0');
      expect(got.latest).toBe('v1.13.0');
    }
  });

  it('never offers a downgrade', () => {
    const got = evaluate('1.14.0', [release('v1.13.0')]);
    expect(got.kind).not.toBe('available');
  });

  it('skips drafts and pre-releases', () => {
    const got = evaluate('1.13.0', [
      release('v2.0.0', { draft: true }),
      release('v1.99.0', { prerelease: true }),
      release('v1.14.0'),
    ]);
    expect(got.kind).toBe('available');
    if (got.kind === 'available') expect(got.latest).toBe('v1.14.0');
  });

  it('is unknown, not up-to-date, when the running version is unparseable', () => {
    const got = evaluate('v1.13.0-2-gabc1234', [release('v1.14.0')]);
    expect(got.kind).toBe('unknown');
  });

  it('is unknown when nothing is published', () => {
    expect(evaluate('1.13.0', []).kind).toBe('unknown');
    expect(evaluate('1.13.0', [release('v1.14.0', { draft: true })]).kind).toBe('unknown');
  });

  it('offers the highest stable version even when a hotfix was created later', () => {
    // GitHub orders /releases by creation time: a backported v1.13.1 tagged
    // after v2.0.0 arrives first in the list and must not shadow it.
    const got = evaluate('1.13.0', [release('v1.13.1'), release('v2.0.0')]);
    expect(got.kind).toBe('available');
    if (got.kind === 'available') expect(got.latest).toBe('v2.0.0');
  });

  it('falls back to the newest release when no tag parses', () => {
    const got = evaluate('1.13.0', [release('nightly'), release('latest-build')]);
    expect(got.kind).toBe('unknown');
  });

  it('separates a release with no binary from an installable one', () => {
    // Still surfaced -- silence would look like "no update exists" -- but as
    // its own kind, because "available" drives an install affordance the
    // operator cannot actually use when CI has attached no image.
    const got = evaluate('1.13.0', [release('v1.14.0', { assets: [] })]);
    expect(got.kind).toBe('no-binary');
    if (got.kind === 'no-binary') expect(got.release.html_url).toContain('v1.14.0');
  });
});

describe('checkForUpdate', () => {
  const ok = (body: unknown): Response =>
    ({ ok: true, status: 200, json: async () => body }) as Response;

  it('queries the right repo and returns a verdict', async () => {
    const doFetch = vi.fn<FetchLike>(async () => ok([release('v1.14.0')]));
    const got = await checkForUpdate('jtn0123/ESP32-Garage-Fan', '1.13.0', doFetch);
    expect(doFetch.mock.calls[0]![0]).toContain(
      'https://api.github.com/repos/jtn0123/ESP32-Garage-Fan/releases',
    );
    expect(got.kind).toBe('available');
  });

  it('degrades to unknown on a network failure rather than throwing', async () => {
    const doFetch = vi.fn<FetchLike>(async () => {
      throw new Error('offline');
    });
    const got = await checkForUpdate('a/b', '1.13.0', doFetch);
    expect(got).toEqual({ kind: 'unknown', reason: 'offline' });
  });

  it('degrades to unknown when rate-limited', async () => {
    const doFetch = vi.fn<FetchLike>(async () => ({ ok: false, status: 403 }) as Response);
    const got = await checkForUpdate('a/b', '1.13.0', doFetch);
    expect(got.kind).toBe('unknown');
    if (got.kind === 'unknown') expect(got.reason).toContain('403');
  });

  it('degrades to unknown when the payload is not an array', async () => {
    const doFetch = vi.fn<FetchLike>(async () => ok({ message: 'Not Found' }));
    expect((await checkForUpdate('a/b', '1.13.0', doFetch)).kind).toBe('unknown');
  });
});

describe('sha256 (plain-JS fallback)', () => {
  // The device serves the console over plain HTTP, where crypto.subtle does
  // not exist, so this implementation IS the integrity check in production.
  // Vectors from FIPS 180-4 and the usual references.
  const hex = (b: Uint8Array) =>
    Array.from(b)
      .map((v) => v.toString(16).padStart(2, '0'))
      .join('');
  const of = (s: string) => hex(sha256(new TextEncoder().encode(s)));

  it('hashes the empty string', () => {
    expect(of('')).toBe('e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
  });

  it('hashes "abc"', () => {
    expect(of('abc')).toBe('ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
  });

  it('hashes the 56-byte vector that straddles the padding boundary', () => {
    expect(of('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq')).toBe(
      '248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1',
    );
  });

  it('hashes a multi-block message (1000 x "a")', () => {
    expect(of('a'.repeat(1000))).toBe(
      '41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3',
    );
  });

  it('agrees with a length that crosses a block boundary exactly (64 bytes)', () => {
    expect(of('a'.repeat(64))).toBe(
      'ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb',
    );
  });

  it('pads minimally at length % 64 == 55', () => {
    // The one case the published vectors miss and the first implementation got
    // wrong: message + 0x80 + 8-byte length lands on exactly one block, so a
    // formula that always adds a block produces a valid-looking buffer with
    // the wrong digest. Found by differential-testing against Node's crypto.
    expect(of('a'.repeat(55))).toBe(
      '9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318',
    );
    expect(of('a'.repeat(119))).toBe(
      '31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb',
    );
  });
});
