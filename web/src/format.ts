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

const WEEKDAYS = ['SUN', 'MON', 'TUE', 'WED', 'THU', 'FRI', 'SAT'] as const;

/**
 * The selected range, as the chart title says it.
 *
 * Ranges under a day are carried as a FRACTION of one (0.25 = 6 h), because
 * everything downstream -- the axis, the scrub stamp, the coverage caption --
 * already reasons in days and reads a fraction correctly without knowing
 * sub-day ranges exist. Only the words have to change.
 */
export function rangeLabel(days: number): string {
  if (days < 1) return `LAST ${Math.round(days * 24)} HOURS`;
  return days === 1 ? 'LAST 24 HOURS' : `LAST ${days} DAYS`;
}

/** The same range as a noun the coverage caption can put "window" after. */
export function rangeNoun(days: number): string {
  return days < 1 ? `${Math.round(days * 24)}-hour` : `${days}-day`;
}

/**
 * The scrubbed instant, at the precision the selected range makes meaningful.
 *
 * On 24H the clock alone is unambiguous. On 7D/30D/60D it is not -- "15:27"
 * happens once a day for sixty days, and the reason to drag across a two-month
 * chart is to find out WHICH day the spike was on. The weekday earns its four
 * characters on the 7-day range, where "8/12" takes a beat to place and "WED"
 * does not; past a week the dates stop being nearby days and it is dropped.
 */
export function moment(epochSeconds: number, days = 1): string {
  const d = new Date(epochSeconds * 1000);
  const time = clock(epochSeconds);
  if (days <= 1) return time;
  const date = `${d.getMonth() + 1}/${d.getDate()}`;
  return days <= 7 ? `${WEEKDAYS[d.getDay()]} ${date} ${time}` : `${date} ${time}`;
}

/**
 * Minutes elapsed -> "45 MIN AGO" / "17H30 AGO" / "3D14H AGO".
 *
 * Days past 48 h, because hours stop being a unit anyone can read there: a
 * 7-day scrub said `86H40 AGO` (three days? four?) and a 30-day one `372H AGO`,
 * which left the date beside it doing all the work. The 48 h threshold keeps
 * "yesterday afternoon" in hours, where hours are still the natural answer.
 */
export function ago(minutes: number): string {
  if (!Number.isFinite(minutes)) return 'AGE UNKNOWN';
  const m = Math.max(0, Math.round(minutes));
  if (m < 60) return `${m} MIN AGO`;
  const hours = Math.floor(m / 60);
  if (hours >= 48) {
    const days = Math.floor(hours / 24);
    const rem = hours % 24;
    const tail = rem === 0 ? '' : `${rem}H`;
    return `${days}D${tail} AGO`;
  }
  const rem = m % 60;
  return `${hours}H${rem ? String(rem).padStart(2, '0') : ''} AGO`;
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

/**
 * One tick label, at the resolution the window can actually distinguish.
 *
 * Three tiers, because one format cannot serve 24 hours and 60 days. A day of
 * samples wants the clock; a week wants the date AND the hour, or two ticks
 * both read "8/3" and the axis looks broken; a month or two wants the date
 * alone -- the hour is noise there (rows are 2.6 to 5.1 hours apart, so "8/3
 * 14h" claims a precision the row does not have) and it is what made the
 * 30D/60D axis unreadable at phone width.
 */
export function axisLabel(t: number, days: number): string {
  const d = new Date(t * 1000);
  const hh = String(d.getHours()).padStart(2, '0');
  if (days <= 1) return `${hh}:${String(d.getMinutes()).padStart(2, '0')}`;
  const date = `${d.getMonth() + 1}/${d.getDate()}`;
  return days <= 7 ? `${date} ${d.getHours()}h` : date;
}
