// Display formatting. Pure, so it is all unit-tested -- these run on every
// SSE frame and a wrong one is the kind of thing you only notice in the garage.

/** Seconds -> "37h12m", the form used for uptime and today's run time. */
export function hoursMinutes(seconds: number): string {
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
  if (speed <= 0) return 'Still';
  if (speed <= 3) return 'Trickle';
  if (speed <= 6) return 'Steady';
  if (speed <= 9) return 'Strong';
  return 'Full tilt';
}

/** Megabytes -> "1.42 / 29.7 GB", matching the status bar and settings row. */
export function storage(usedMb: number, totalMb: number): string {
  return `${(usedMb / 1024).toFixed(2)} / ${(totalMb / 1024).toFixed(1)} GB`;
}
