// The wire contract between fan_controller_main.cpp and this page.
//
// Every field here is written by a hand-rolled snprintf on the firmware side.
// There is no schema keeping the two in step, so this file IS the contract:
// if you rename a field in state_json(), rename it here and the compiler will
// find every reader. Before this existed, a renamed field showed up as a
// silent "-" in the UI with no error anywhere.
//
// `| null` is used where the firmware literally emits the JSON token `null`
// (a missing sensor, no outdoor feed, no battery). Optional `?` is used where
// the firmware omits the key entirely.
//
// /api/history used to be the example of that second case: its 7- and 30-day
// responses carried temperature, humidity and pressure only. They now carry
// the same seven series and a real per-row timestamp as the 24 h range, and
// tests/test_web_contract.py::test_history_branches_agree keeps both firmware
// branches emitting the identical set.

/** GET /api/state -- also the payload pushed over SSE on port 8081. */
export interface DeviceState {
  speed: number; // 0..12, or negative while /api/raw holds a manual duty
  auto: boolean;
  auto_max: number; // speed auto holds above the engage threshold
  auto_min: number; // speed auto rests at once equalised (0 = off)
  on_f: number; // engage differential, degrees F
  off_f: number; // release differential, degrees F
  outside_f: number | null; // null when the yard feed is missing or stale
  toff: number; // the probe offset currently in effect, degrees C
  offc: number; // offset applied while charging
  offi: number; // offset applied while idle
  fw: string; // FW_VERSION -- the update check compares exactly this
  slot: string; // "app0" | "app1"
  confirmed: boolean;
  unhealthy_boots: number;
  sensor: boolean; // BME280 answered at boot
  last_reset: string;
  boots: number;
  prev_death: string;
  sd_q: boolean; // card quarantined after a mount crashed a boot
  sd_total_mb: number; // 0 when no card is mounted
  sd_used_mb: number;
  /**
   * Free space, MB. Surfaced because the deployed card sat at 210 MB free of
   * 28887 -- 99.3% full with data this firmware never wrote -- and nothing
   * reported it, so nothing could notice.
   */
  sd_free_mb: number;
  batt: BatteryState | null;
  rssi: number;
  drops: number; // WiFi disconnects since boot -- the flight recorder's counter
  mqtt: boolean;
  uptime_s: number;
  ip: string;
  /**
   * The Tapo watt meter on the fan's supply, polled out of Home Assistant by
   * the firmware (net/plug) -- the fan link's only feedback. Null when the
   * poller is disabled (no HA credentials baked) or has never read.
   */
  plug: PlugState | null;
  /** True (default): the off-board SHT41 drives temp/RH when it answers. */
  sht_pref: boolean;
  /** Gas boost: in auto mode a high VOC index forces a minimum speed. */
  gas_on: boolean;
  /** The floor speed enforced while the VOC latch holds (1-12). */
  gas_spd: number;
  /** VOC index that latches the boost (release is 50 below, floor 100). */
  gas_voc: number;
  /** True while the latch is holding the floor right now. */
  gas_active: boolean;
}

export interface PlugState {
  w: number; // measured draw, watts
  v: number | null; // mains voltage; null if HA has no voltage entity
  age_s: number; // seconds since the reading
  /**
   * Does the measured draw agree with the commanded speed?
   * 1 agree, -1 sustained disagreement (the "commanded 0 W but the meter says
   * 40 W" failure that ran the fan for a day, 2026-08-13), 0 cannot say
   * (stale reading, speed changed too recently).
   */
  verdict: -1 | 0 | 1;
}

export interface BatteryState {
  v: number;
  pct: number | null; // null on the raw-register LC709203F path
  chg: boolean;
  eta_h: number | null; // null while charging or flat
  mvh: number | null; // null until an hour of samples exists
}

/**
 * GET /api/device -- facts that never change while the board is up.
 * Fetched once per page load, deliberately kept out of /api/state so the 15 s
 * poll and every SSE frame stay small.
 */
export interface DeviceInfo {
  id: string;
  host: string;
  repo: string; // "owner/name" on GitHub -- drives the update check
  broker: string;
  ssid: string;
  topic_set: string;
  topic_out: string;
  period_us: number; // PWM frame, measured: 9934
  sample_s: number;
  high_us: number[]; // index 0..12, the measured duty table
}

/** GET /api/stats */
export interface Stats {
  run_today_s: number;
  run_total_s: number;
  energy_wh: number;
  watts_now: number;
  t_min_f: number;
  t_max_f: number;
  t_avg_f: number;
  samples: number; // 0 means the ring is empty; the t_* fields are then 0
}

/** GET /api/sensors -- `ok:false` when the BME280 is missing or the ring is empty. */
export type Sensors =
  | { ok: false }
  | {
      ok: true;
      temp_c: number;
      rh: number;
      hpa: number;
      /** Which sensor produced temp/rh: the off-board SHT41 or the BME280 fallback. */
      source: 'sht41' | 'bme280';
      /** SGP41 raw ticks, meaningful from the first second; -1 = no sensor. */
      voc_raw: number;
      nox_raw: number;
      /** Sensirion gas indices 1..500; 0 = still warming up, -1 = no sensor. */
      voc: number;
      nox: number;
      /** The BME280's own estimate, always present, for the live comparison. */
      bme_t: number;
      bme_rh: number;
    };

/**
 * The 4xx body every endpoint sends for a malformed request. api.ts throws
 * on non-2xx before parsing, so the console never sees this shape -- it is
 * declared for the wire contract (the firmware emits it) and for anyone
 * driving the API by hand.
 */
export interface ApiError {
  error: string;
}

/**
 * GET /api/history?days=1|7|30
 *
 * `days` is REQUIRED and must be exactly 1, 7 or 30; anything else is a 400
 * ApiError. (It used to default to 1, which served RAM-ring data to typo'd
 * queries as if it were the card's -- 2026-08-10.)
 *
 * Every range now returns the SAME shape, all seven series plus a real
 * per-row `ts`. Previously the 7/30-day responses came out of a code path that
 * emitted temperature, humidity and pressure only, with no timestamp at all --
 * so switching the range dropped four of the seven lines and left the chart
 * spacing whatever rows existed evenly across the whole requested window.
 *
 * A range the device cannot answer (card unmounted, clock unsynced) is now a
 * 503 ApiError rather than ring data wearing the requested range's label.
 */
export interface History {
  /**
   * Nominal sample spacing, for gap detection only. `ts` is the authority on
   * where each row actually sits; this used to be days*86400/rows, which
   * inflated by requested_span/actual_span whenever the card held less than
   * the window asked for.
   */
  interval_s: number;
  /** Which store answered: the SD card, or the in-RAM ring (no card yet). */
  source: 'sd' | 'ring';
  /** Epoch seconds per row, oldest first. 0 entries mean SNTP had not synced. */
  ts: number[];
  temp_c: (number | null)[];
  rh: (number | null)[];
  hpa: (number | null)[];
  out_f: (number | null)[]; // -999 sentinel is normalised to null on ingest
  spd: number[];
  batt_v: (number | null)[];
  chg: number[]; // 1 charging, 0 not, -1 no battery
  /** Fan draw measured at the plug; null before the meter existed (pre-1.14.47 rows). */
  watts: (number | null)[];
  /** SGP41 raw ticks -- meaningful from the first sample; null = no sensor / old row. */
  voc_raw: (number | null)[];
  nox_raw: (number | null)[];
  /** Sensirion gas indices 1..500. 0 = the algorithm was still warming up. */
  voc: (number | null)[];
  nox: (number | null)[];
  /** The BME280 logged beside the SHT41, for the dual-sensor overlays. */
  bme_t: (number | null)[];
  bme_rh: (number | null)[];
}

/**
 * `POST /api/sdpurge` — deleting the card's contents without reformatting.
 *
 * Declared here so the wire-contract test holds this endpoint to the same rule
 * as the rest: the firmware and the console cannot drift apart silently.
 *
 * `complete` is not simply "ok". It means nothing foreign is left, no bound was
 * hit, AND the verification pass actually ran; `note` explains which of those
 * failed. `remaining`/`leftover` name what survived (in practice macOS's
 * `.Spotlight-V100`, which FAT will not remove and re-running cannot fix), and
 * `skipped_dirs` is the walk-queue bound, which is NOT the entry budget.
 */
export interface PurgeResult {
  ok: boolean;
  files: number;
  dirs: number;
  remaining: number;
  skipped_dirs: number;
  /** Entries whose path exceeded the walk's buffer; skipped rather than risked. */
  too_long: number;
  leftover: string;
  free_before_mb: number;
  free_mb: number;
  complete: boolean;
  /** The device reboots after a purge; see handle_sd_purge for why. */
  restarting: boolean;
  note: string;
}

/**
 * `GET /api/display` — the e-ink panel's last frame, for the console's mirror.
 *
 * Two 1-bit planes, base64'd, row-major, MSB first, a set bit meaning ink. This
 * is the firmware's own format, NOT the display driver's: Adafruit_EPD stores
 * its buffer column-major and rotated with per-driver inversion, so decoding
 * that here would make the mirror a re-derivation of a private implementation
 * detail — one that renders something plausible and wrong the first time the
 * driver changes.
 *
 * These are the same bytes that were clocked to the glass, so the picture in
 * the browser cannot drift from the picture on the wall. That is the whole
 * reason this is a framebuffer and not a JS re-implementation of the layout.
 */
export interface DisplayFrame {
  /** False until the panel has painted once; every other field is then empty. */
  ready: boolean;
  w: number;
  h: number;
  /** Bytes per row: (w + 7) / 8. */
  stride: number;
  /**
   * Whether the fitted panel can actually show the red plane.
   *
   * Both the mono and tricolor 2.13" FeatherWings are SSD1680 at 250×122, so
   * the firmware is identical and only the glass differs. Everything red is an
   * accent over information already legible in black, so a mono panel loses
   * decoration and no meaning — but the mirror renders red as grey when this is
   * false, because the point is to show what THIS device displays.
   */
  tricolor: boolean;
  /** Seconds since the last refresh; -1 when it has never painted. */
  age_s: number;
  /** base64 of the black plane. */
  black: string;
  /** base64 of the red plane. Drawn over black; a pixel may be in both. */
  red: string;
}
