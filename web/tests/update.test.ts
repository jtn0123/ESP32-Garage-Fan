import { describe, expect, it, vi } from 'vitest';
import {
  checkForUpdate,
  evaluate,
  pickChecksumAsset,
  pickFirmwareAsset,
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

  it('reports up to date when the device is ahead', () => {
    expect(evaluate('1.14.0', [release('v1.13.0')]).kind).toBe('up-to-date');
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

  it('still reports available when the release has no attached binary', () => {
    // The banner should link to the release page even if CI has not finished
    // attaching the image -- silence would look like "no update exists".
    const got = evaluate('1.13.0', [release('v1.14.0', { assets: [] })]);
    expect(got.kind).toBe('available');
    if (got.kind === 'available') expect(got.asset).toBeNull();
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
