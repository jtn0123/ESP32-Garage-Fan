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
// the firmware omits the key entirely -- which /api/history does for the
// 7-day and 30-day ranges, because those come off the SD card and it only
// stores temperature, humidity and pressure.

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
  batt: BatteryState | null;
  rssi: number;
  drops: number; // WiFi disconnects since boot -- the flight recorder's counter
  mqtt: boolean;
  uptime_s: number;
  ip: string;
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
export type Sensors = { ok: false } | { ok: true; temp_c: number; rh: number; hpa: number };

/**
 * GET /api/history?days=1|7|30
 *
 * The optional series are present only for days=1, which is served from the
 * in-RAM ring. Longer ranges are decimated out of the monthly CSVs on the SD
 * card, which carry temperature, humidity and pressure only.
 */
export interface History {
  interval_s: number;
  /**
   * Present (0 before SNTP has synced) on the 24 h ring response; the 7/30-day
   * SD responses omit it entirely -- their rows are decimated from month files
   * and carry no anchor timestamp.
   */
  end_ts?: number;
  temp_c: (number | null)[];
  rh: (number | null)[];
  hpa: (number | null)[];
  out_f?: (number | null)[]; // -999 sentinel is normalised to null on ingest
  spd?: number[];
  batt_v?: (number | null)[];
  chg?: number[]; // 1 charging, 0 not, -1 no battery
}
