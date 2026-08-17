// The console's single mutable view-model, shared between the wiring in
// app.ts and the painters in console.ts. One object, re-derived DOM: state
// arrives by SSE (or the 15 s poll) and every command endpoint answers with
// the new state, so nothing here optimistically guesses what the device did.

import type { BootMark, DeviceInfo, DeviceState, History, Sensors, Stats } from './types.js';
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
  /** Restart marks inside the loaded window; [] when unknown or none. */
  boots: BootMark[];
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
  /**
   * Consecutive failed state polls, 0 while the device is answering.
   *
   * The offline VERDICT waits for several of these; this is what lets the page
   * stop saying `NOW` on the very first one, so the decay is visible before the
   * verdict lands rather than 45 seconds of confident-looking numbers.
   */
  pollFail: number;
  /**
   * When this page load started, as the fallback reference for `pollFail`.
   *
   * The offline check used to be gated on `lastOk`, which is 0 until a poll
   * succeeds -- so a console opened COLD against a dead controller never went
   * offline at all and sat on "Connecting to the controller…" forever. That is
   * the common case: you walk into the garage and open the page because the fan
   * is not running.
   */
  startedAt: number;
  /** Speed under a live rail drag, or null. Outranks the device's own speed. */
  railPick: number | null;
}

export const view: View = {
  state: null,
  info: null,
  stats: null,
  live: null,
  series: null,
  history: null,
  boots: [],
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
  pollFail: 0,
  startedAt: Date.now(),
  railPick: null,
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

/** One status bit: keeps each entry a single call so the list reads as a table. */
function bit(key: string, title: string, body: string) {
  return { key, title, body } as const;
}

export const STATUS_BITS = [
  bit('RSSI', 'WI-FI SIGNAL',
    'How strong the controller’s link to the house access point is. Around −61 dBm is a solid connection; below about −80 dBm the MQTT session starts dropping and telemetry gaps show up in the charts.'),
  bit('SD', 'LOG STORAGE',
    'Space used by on-device logs on the microSD card. Every 5-minute sample is written locally first, then published — so a broker outage never loses history. The card is mounted lazily and quarantined if a mount ever crashes the board.'),
  bit('VBAT', 'BATTERY VOLTAGE',
    'Live cell voltage of the backup pack — the same series plotted in the BATTERY row. Roughly 4.10 V is full and 3.3 V is empty. The pack keeps the controller logging when the USB supply drops out; the fan itself runs from its own 12 V brick.'),
  bit('PLUG', 'MEASURED DRAW',
    'What the Tapo watt meter on the fan’s supply actually measures, polled out of Home Assistant by the controller. This is the fan link’s only feedback — the control wire has no tach — so a draw that disagrees with the commanded speed is the one signal that catches a fan not obeying.'),
  bit('GAS', 'GAS BOOST',
    'Bad air overrides a resting thermostat: while the SGP41’s VOC index is above the trigger, auto mode holds at least the boost speed — a car started in the garage spins the fan up even when the temperature says rest. Trigger, speed and the on/off switch live in Settings › Auto mode.'),
  bit('FW', 'RUNNING FIRMWARE',
    'The image this controller is currently executing. The header carries it too on a wide screen; on a phone it lives here, beside the slot it booted from. Settings › Update compares it against the latest published release and can flash a new one over the air.'),
  bit('UPTIME', 'TIME SINCE BOOT',
    'How long the controller has been running without a reboot. A number that keeps resetting to zero usually means brownouts on the supply rather than a firmware crash — the status line also carries the previous boot’s cause of death.'),
  bit('SLOT', 'FIRMWARE SLOT',
    'The device keeps two firmware images, app0 and app1, and boots whichever was flashed last. “Confirmed” means this image came up cleanly and checked in with the broker, so it is now the permanent choice — an unconfirmed image rolls back to the other slot rather than boot-looping.'),
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
