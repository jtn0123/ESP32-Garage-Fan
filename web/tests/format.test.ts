import { describe, expect, it } from 'vitest';
import { ago, airflow, clock, hoursMinutes, moment, signed, storage } from '../src/format.js';

describe('hoursMinutes', () => {
  it('formats the uptime style', () => {
    expect(hoursMinutes(133920)).toBe('37h12m');
    expect(hoursMinutes(0)).toBe('0h00m');
    expect(hoursMinutes(59)).toBe('0h00m');
    expect(hoursMinutes(3600)).toBe('1h00m');
  });

  it('never goes negative', () => {
    expect(hoursMinutes(-5)).toBe('0h00m');
  });
});

describe('ago', () => {
  it('uses minutes under an hour', () => {
    expect(ago(45)).toBe('45 MIN AGO');
    expect(ago(0)).toBe('0 MIN AGO');
  });

  it('switches to hours with zero-padded minutes', () => {
    expect(ago(60)).toBe('1H AGO');
    expect(ago(1050)).toBe('17H30 AGO');
    expect(ago(61)).toBe('1H01 AGO');
  });
});

describe('signed', () => {
  it('always carries a sign on positives', () => {
    expect(signed(3.44)).toBe('+3.4');
    expect(signed(-1.26)).toBe('-1.3');
    expect(signed(0)).toBe('+0.0');
  });
});

describe('airflow', () => {
  it('maps every speed to a word', () => {
    expect(airflow(0)).toBe('Still');
    expect(airflow(3)).toBe('Trickle');
    expect(airflow(6)).toBe('Steady');
    expect(airflow(9)).toBe('Strong');
    expect(airflow(12)).toBe('Full tilt');
  });

  it('treats the raw sentinel as still', () => {
    expect(airflow(-1)).toBe('Still');
  });
});

describe('storage', () => {
  it('renders used / total in GB', () => {
    expect(storage(1454, 30412)).toBe('1.42 / 29.7 GB');
  });
});

describe('clock', () => {
  it('zero-pads both fields', () => {
    // Construct a local-time epoch so the test passes in any timezone.
    const d = new Date(2026, 7, 8, 4, 5);
    expect(clock(d.getTime() / 1000)).toBe('04:05');
  });
});

describe('moment: the scrubbed instant', () => {
  const t = Math.floor(new Date(2026, 7, 16, 15, 27).getTime() / 1000); // a Sunday

  it('is just the clock on the 24 h range', () => {
    expect(moment(t, 1)).toBe('15:27');
  });

  it('names the day on a week, where 8/16 takes a beat to place', () => {
    expect(moment(t, 7)).toBe('SUN 8/16 15:27');
  });

  it('keeps the date but drops the weekday past a week', () => {
    // Sixty days back, "SUN" narrows nothing -- there are nine of them.
    expect(moment(t, 60)).toBe('8/16 15:27');
  });
});
