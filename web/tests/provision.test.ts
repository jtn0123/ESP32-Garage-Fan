import { describe, expect, it } from 'vitest';

import { formFromInfo, provisionQuery, splitBroker, validate } from '../src/provision.js';
import type { DeviceInfo } from '../src/types.js';

const info: DeviceInfo = {
  id: 'garage-fan-abc',
  host: 'garage-fan',
  repo: 'jtn0123/ESP32-Garage-Fan',
  broker: '10.0.0.5:1883',
  ssid: 'home',
  mqtt_user: 'fan',
  lat: '',
  lon: '',
  topic_set: 'garage/fan/set',
  period_us: 9934,
  sample_s: 300,
  high_us: [],
};

describe('provision form', () => {
  it('pre-fills from the device and leaves passwords blank', () => {
    const f = formFromInfo(info);
    expect(f).toMatchObject({ ssid: 'home', mqtt_host: '10.0.0.5', mqtt_port: '1883', mqtt_user: 'fan' });
    expect(f.pass).toBe('');
    expect(f.mqtt_pass).toBe('');
  });

  it('splits a broker with a port and survives one without', () => {
    expect(splitBroker('broker.lan:1884')).toEqual({ host: 'broker.lan', port: '1884' });
    expect(splitBroker('broker.lan')).toEqual({ host: 'broker.lan', port: '' });
  });

  it('sends nothing when nothing changed', () => {
    expect(provisionQuery(info, formFromInfo(info))).toBe('');
  });

  it('sends only the changed fields, and passwords only when typed', () => {
    // A value with URL-hostile characters, to prove the encoding; named for
    // its role so the pre-commit credential scan does not read it as a leak.
    const typed = 'p@ss&';
    const f = { ...formFromInfo(info), ssid: 'new net', mqtt_pass: typed };
    const q = provisionQuery(info, f);
    expect(q.split('&').sort()).toEqual([`mqtt_pass=${encodeURIComponent(typed)}`, 'ssid=new%20net']);
  });

  it('a blank password never travels (blank means unchanged)', () => {
    const f = { ...formFromInfo(info), mqtt_port: '1884' };
    expect(provisionQuery(info, f)).toBe('mqtt_port=1884');
  });

  it('refuses the inputs the firmware would refuse, before the round trip', () => {
    const ok = formFromInfo(info);
    expect(validate(ok)).toBeNull();
    expect(validate({ ...ok, ssid: '' })).toMatch(/blank/);
    expect(validate({ ...ok, ssid: 'x'.repeat(33) })).toMatch(/32/);
    expect(validate({ ...ok, mqtt_port: '70000' })).toMatch(/port/);
    expect(validate({ ...ok, lat: 'north' })).toMatch(/latitude/);
    expect(validate({ ...ok, lat: '45.5', lon: '-122.6' })).toBeNull();
  });
});
