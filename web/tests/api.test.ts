import { afterEach, describe, expect, it, vi } from 'vitest';
import { provision, restart, setConfig, uploadFirmware } from '../src/api.js';

afterEach(() => vi.restoreAllMocks());

describe('credential transport', () => {
  it('sends provisioning passwords in the body and authorization in a header', async () => {
    const fetcher = vi.spyOn(globalThis, 'fetch').mockResolvedValue(new Response('{"ok":true}'));
    expect(await provision('pass=+wifi+&mqtt_pass=+mqtt+', 'test-only-token')).toEqual({ ok: true });
    const [url, init] = fetcher.mock.calls[0]!;
    expect(url).toBe('/api/provision');
    expect(init?.headers).toEqual({
      'Content-Type': 'application/x-www-form-urlencoded', 'X-Fan-Token': 'test-only-token',
    });
    const body = new URLSearchParams(init?.body as string);
    expect(body.get('pass')).toBe(' wifi ');
    expect(body.get('mqtt_pass')).toBe(' mqtt ');
  });

  it('sends both token rotation credentials outside the URL', async () => {
    const fetcher = vi.spyOn(globalThis, 'fetch').mockResolvedValue(new Response('{}'));
    await setConfig('newtoken=next-test-token&auth=current-test-token');
    const [url, init] = fetcher.mock.calls[0]!;
    expect(url).toBe('/api/config');
    expect((init?.headers as Record<string, string>)['X-Fan-Token']).toBe('current-test-token');
    expect(new URLSearchParams(init?.body as string).get('newtoken')).toBe('next-test-token');
    expect(new URLSearchParams(init?.body as string).has('auth')).toBe(false);
  });

  it('uses headers for maintenance and multipart firmware uploads', async () => {
    const fetcher = vi.spyOn(globalThis, 'fetch').mockImplementation(async () => new Response('{}'));
    await restart('test-only-token');
    await uploadFirmware(new File(['image'], 'firmware.bin'), 'test-only-token');
    expect(fetcher.mock.calls.map(([url]) => url)).toEqual(['/api/restart', '/update']);
    for (const [, init] of fetcher.mock.calls) {
      expect(init?.headers).toEqual({ 'X-Fan-Token': 'test-only-token' });
    }
    expect(fetcher.mock.calls[1]![1]?.body).toBeInstanceOf(FormData);
  });
});
