// The console's single mutable view-model, shared between the wiring in
// app.ts and the painters in console.ts. One object, re-derived DOM: state
// arrives by SSE (or the 15 s poll) and every command endpoint answers with
// the new state, so nothing here optimistically guesses what the device did.

import type { DeviceInfo, DeviceState, History, Sensors, Stats } from './types.js';
import type { Series } from './series.js';
import type { UpdateStatus } from './update.js';

export type RowKey = 'fan' | 'humidity' | 'pressure' | 'battery' | 'power' | 'voc' | 'nox';

export interface View {
  state: DeviceState | null;
  info: DeviceInfo | null;
  stats: Stats | null;
  live: Sensors | null;
  series: Series | null;
  history: History | null;
  /** Why the last history fetch failed, or null. Shown in place of the chart. */
  historyError: string | null;
  update: UpdateStatus | null;
  days: number;
  scrub: number;
  screen: 'console' | 'settings';
  scopeOpen: boolean;
  previewOpen: boolean;
  tip: number;
  rows: Record<RowKey, boolean>;
  phase: number;
  raf: number | null;
  /**
   * Date.now() of the last state we successfully received, 0 if never.
   *
   * Nothing used to record this, so nothing could tell a live page from a dead
   * one: unplugging the controller left "● BROKER UP", the speed rail, the
   * hero temperature under its `NOW` label and a frozen UPTIME on screen
   * indefinitely, visually identical to a working device.
   */
  lastOk: number;
  /** True once contact has been lost long enough to distrust what is shown. */
  offline: boolean;
}

export const view: View = {
  state: null,
  info: null,
  stats: null,
  live: null,
  series: null,
  history: null,
  historyError: null,
  update: null,
  days: 1,
  scrub: -1,
  screen: 'console',
  scopeOpen: false,
  previewOpen: false,
  tip: -1,
  rows: { fan: true, humidity: false, pressure: false, battery: false, power: false, voc: false, nox: false },
  phase: 0,
  raf: null,
  lastOk: 0,
  offline: false,
};

export const ROW_IDS: Record<RowKey, { sub: string; canvas: string; readout: string }> = {
  fan: { sub: 'sub_s', canvas: 'cv_s', readout: 'roS' },
  humidity: { sub: 'sub_h', canvas: 'cv_h', readout: 'roH' },
  pressure: { sub: 'sub_p', canvas: 'cv_p', readout: 'roP' },
  battery: { sub: 'sub_b', canvas: 'cv_b', readout: 'roB' },
  power: { sub: 'sub_w', canvas: 'cv_w', readout: 'roW' },
  voc: { sub: 'sub_v', canvas: 'cv_v', readout: 'roV' },
  nox: { sub: 'sub_n', canvas: 'cv_n', readout: 'roN' },
};

export const STATUS_BITS = [
  {
    key: 'RSSI',
    title: 'WI-FI SIGNAL',
    body: 'How strong the controller’s link to the house access point is. Around −61 dBm is a solid connection; below about −80 dBm the MQTT session starts dropping and telemetry gaps show up in the charts.',
  },
  {
    key: 'SD',
    title: 'LOG STORAGE',
    body: 'Space used by on-device logs on the microSD card. Every 5-minute sample is written locally first, then published — so a broker outage never loses history. The card is mounted lazily and quarantined if a mount ever crashes the board.',
  },
  {
    key: 'VBAT',
    title: 'BATTERY VOLTAGE',
    body: 'Live cell voltage of the backup pack — the same series plotted in the BATTERY row. Roughly 4.10 V is full and 3.3 V is empty. The pack keeps the controller logging when the USB supply drops out; the fan itself runs from its own 12 V brick.',
  },
  {
    key: 'PLUG',
    title: 'MEASURED DRAW',
    body: 'What the Tapo watt meter on the fan’s supply actually measures, polled out of Home Assistant by the controller. This is the fan link’s only feedback — the control wire has no tach — so a draw that disagrees with the commanded speed is the one signal that catches a fan not obeying.',
  },
  {
    key: 'T-OFFSET',
    title: 'PROBE CALIBRATION',
    body: 'A fixed correction added to every garage temperature reading to cancel heat the board adds to its own sensor. Charging warms the board much more than idle does, so there are two values — set both in Settings › Sensors after comparing against a reference thermometer.',
  },
  {
    key: 'UPTIME',
    title: 'TIME SINCE BOOT',
    body: 'How long the controller has been running without a reboot. A number that keeps resetting to zero usually means brownouts on the supply rather than a firmware crash — the status line also carries the previous boot’s cause of death.',
  },
  {
    key: 'SLOT',
    title: 'FIRMWARE SLOT',
    body: 'The device keeps two firmware images, app0 and app1, and boots whichever was flashed last. “Confirmed” means this image came up cleanly and checked in with the broker, so it is now the permanent choice — an unconfirmed image rolls back to the other slot rather than boot-looping.',
  },
] as const;

/**
 * The row index the readouts describe: the scrub position while the cursor
 * is on the chart, otherwise the newest sample.
 */
export function sampleIndex(): number {
  const s = view.series;
  if (!s || !s.n) return -1;
  return view.scrub >= 0 ? view.scrub : s.n - 1;
}
