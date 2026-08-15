// Display formatting. Pure, so it is all unit-tested -- these run on every
// SSE frame and a wrong one is the kind of thing you only notice in the garage.

/** Seconds -> "37h12m", the form used for uptime and today's run time. */
export function hoursMinutes(seconds: number): string {
  // Math.max(0, NaN) is NaN, so the old guard let "NaNhNaNm" onto the status
  // line. Every formatter here takes device-supplied numbers and must render
  // SOMETHING truthful for a value that is not a number.
  if (!Number.isFinite(seconds)) return '–';
  const s = Math.max(0, Math.floor(seconds));
  return `${Math.floor(s / 3600)}h${String(Math.floor((s % 3600) / 60)).padStart(2, '0')}m`;
}

/** Epoch seconds -> "04:05" in the viewer's local zone. */
export function clock(epochSeconds: number): string {
  const d = new Date(epochSeconds * 1000);
  return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
}

/** Minutes elapsed -> "45 MIN AGO" / "17H30 AGO", for the scrub stamp. */
export function ago(minutes: number): string {
  if (!Number.isFinite(minutes)) return 'AGE UNKNOWN';
  const m = Math.max(0, Math.round(minutes));
  if (m < 60) return `${m} MIN AGO`;
  const rem = m % 60;
  return `${Math.floor(m / 60)}H${rem ? String(rem).padStart(2, '0') : ''} AGO`;
}

/** Always-signed one-decimal number, for the differential readout. */
export function signed(v: number, digits = 1): string {
  return (v >= 0 ? '+' : '') + v.toFixed(digits);
}

/** Speed 0..12 -> the word shown under AIRFLOW. */
export function airflow(speed: number): string {
  // NaN fails EVERY comparison below, so an unknown speed used to fall
  // through to "Full tilt" -- a missing reading reading as maximum airflow
  // is the worst possible direction for this particular guess to go.
  if (!Number.isFinite(speed)) return 'Unknown';
  if (speed <= 0) return 'Still';
  if (speed <= 3) return 'Trickle';
  if (speed <= 6) return 'Steady';
  if (speed <= 9) return 'Strong';
  return 'Full tilt';
}

/**
 * Megabytes -> "1.42 / 29.7 GB", with the free space named once it gets tight.
 *
 * "28.00 / 28.2 GB" is technically the same information and conveys nothing:
 * the deployed card sat at 210 MB free and read as unremarkable. The whole
 * reason sd_free_mb was added to the wire is that a nearly-full card was
 * invisible, so the formatter has to actually say it.
 */
export function storage(usedMb: number, totalMb: number, freeMb?: number): string {
  if (!Number.isFinite(usedMb) || !Number.isFinite(totalMb)) return '–';
  const base = `${(usedMb / 1024).toFixed(2)} / ${(totalMb / 1024).toFixed(1)} GB`;
  // freeMb is RENDERED below, so it needs the same guard the other two
  // got: a non-finite value reached the user as 'NaN MB free'.
  if (freeMb === undefined || !Number.isFinite(freeMb) || totalMb <= 0) return base;
  const pctFull = (100 * usedMb) / totalMb;
  if (pctFull < 90) return base;
  const free = freeMb >= 1024 ? `${(freeMb / 1024).toFixed(1)} GB` : `${freeMb} MB`;
  return `${base} · ${free} free (${pctFull.toFixed(0)}% full)`;
}

/** True once the card is full enough that logging is at risk. */
export function cardTight(usedMb: number, totalMb: number): boolean {
  return totalMb > 0 && (100 * usedMb) / totalMb >= 90;
}
