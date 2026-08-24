// @vitest-environment node
import { describe, expect, it } from 'vitest';

import type { DeviceState } from '../src/types.js';
import type { UpdateStatus } from '../src/update.js';
import { installUpdate, judgeReboot, pagesUrl, type InstallDeps } from '../src/updater.js';
import { sha256 } from '../src/update.js';

const hex = (b: Uint8Array) => [...b].map((v) => v.toString(16).padStart(2, '0')).join('');

describe('pagesUrl', () => {
  it('builds the channel URL release.yml publishes to', () => {
    expect(pagesUrl('jtn0123/ESP32-Garage-Fan', 'garage-fan-v1.2.3.bin')).toBe(
      'https://jtn0123.github.io/ESP32-Garage-Fan/firmware/garage-fan-v1.2.3.bin',
    );
  });
});

describe('judgeReboot', () => {
  const before = { fw: '1.18.0', boots: 84 };
  it('waits while the old image is still the one answering', () => {
    expect(judgeReboot(before, '1.19.0', { fw: '1.18.0', boots: 84, confirmed: true })).toBe('waiting');
  });
  it('sees the new image running but not yet confirmed', () => {
    expect(judgeReboot(before, '1.19.0', { fw: '1.19.0', boots: 85, confirmed: false })).toBe('unconfirmed');
  });
  it('confirms only once the broker has been reached', () => {
    expect(judgeReboot(before, '1.19.0', { fw: '1.19.0', boots: 85, confirmed: true })).toBe('confirmed');
  });
  it('calls a reboot onto the old version a rollback', () => {
    expect(judgeReboot(before, '1.19.0', { fw: '1.18.0', boots: 88, confirmed: true })).toBe('rolled-back');
  });
});

describe('installUpdate', () => {
  const image = new TextEncoder().encode('FAKE-FIRMWARE');
  const sum = hex(sha256(image));
  const status: Extract<UpdateStatus, { kind: 'available' }> = {
    kind: 'available',
    latest: 'v9.9.9',
    release: {
      tag_name: 'v9.9.9', name: 'Release v9.9.9', html_url: 'https://example.invalid/r', body: '',
      draft: false, prerelease: false, assets: [],
    },
    asset: { name: 'garage-fan-v9.9.9.bin', browser_download_url: 'https://example.invalid/x', size: image.length },
  };
  const state = (over: Partial<DeviceState>): DeviceState =>
    ({ fw: '1.18.0', boots: 84, confirmed: true, ...over }) as DeviceState;

  function deps(over: Partial<InstallDeps> & { checksum?: string; states?: DeviceState[] }): InstallDeps & { log: string[]; uploads: string[] } {
    const log: string[] = [];
    const uploads: string[] = [];
    const states = over.states ?? [state({ fw: '9.9.9', boots: 85, confirmed: true })];
    return {
      log,
      uploads,
      fetch: async (url) =>
        url.endsWith('.sha256')
          ? new Response(`${over.checksum ?? sum}  garage-fan-v9.9.9.bin\n`)
          : new Response(image),
      upload: async (_f, token) => {
        uploads.push(token);
        return token === 'iliving-ota' ? '{"ok":true}' : '{"error":"bad token"}';
      },
      getState: async () => states.shift() ?? state({ fw: '9.9.9', boots: 85, confirmed: true }),
      state: () => state({}),
      askToken: () => null,
      say: (t) => log.push(t),
      sleep: async () => {},
      confirmMs: 10_000,
      ...over,
    };
  }

  it('downloads, verifies, uploads and reports confirmed', async () => {
    const d = deps({});
    const final = await installUpdate(status, 'o/r', 'iliving-ota', d);
    expect(final).toMatch(/updated to v9.9.9 and confirmed/);
    expect(d.uploads).toEqual(['iliving-ota']);
    expect(d.log.join(' | ')).toMatch(/downloading .* checking .* verified .* uploaded/);
  });

  it('a checksum mismatch aborts before anything is sent', async () => {
    const d = deps({ checksum: '0'.repeat(64) });
    const final = await installUpdate(status, 'o/r', 'iliving-ota', d);
    expect(final).toMatch(/ABORTED.*checksum/);
    expect(d.uploads).toEqual([]);
  });

  it('a missing channel file is explained, not thrown', async () => {
    const d = deps({ fetch: async () => new Response('', { status: 404 }) });
    expect(await installUpdate(status, 'o/r', 'iliving-ota', d)).toMatch(/not on the update channel yet/);
    expect(d.uploads).toEqual([]);
  });

  it('a refused token asks once, then gives up plainly', async () => {
    const d = deps({ askToken: () => null });
    expect(await installUpdate(status, 'o/r', 'wrong', d)).toMatch(/refused the update token/);
    expect(d.uploads).toEqual(['wrong']);
  });

  it('a reboot back onto the old version is reported as a rollback', async () => {
    const d = deps({ states: [state({ fw: '1.18.0', boots: 84 }), state({ fw: '1.18.0', boots: 87 })] });
    expect(await installUpdate(status, 'o/r', 'iliving-ota', d)).toMatch(/rolled back/);
  });
});
