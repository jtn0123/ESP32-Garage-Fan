//! Reading the flight recorder.
//!
//! `/events.log` is one record per line:
//!
//! ```text
//! 1787861886 266426 plug window sp0/~0 pad=0% w=1.3/1.4/1.7 on=0 off=0 new=20 miss=0 cyc=0
//! 1787862036 266576 auto in=83.7 out=97.1 d=-13.4 latch=off dwell=0/30 tgt=0 gas=off
//! ```
//!
//! epoch seconds, uptime seconds, a tag, then the message. Only the lines that
//! answer "is the fan doing what it was told" are given a type here; everything
//! else is kept as `Other` so the noise can be counted rather than discarded --
//! the recorder holds 24 lines in RAM, so what is crowding it out is itself a
//! finding.

/// One `plug window` line: the five-minute summary of what the meter saw.
#[derive(Debug, Clone, PartialEq)]
pub struct Window {
    pub epoch: i64,
    /// Speed the firmware commanded.
    pub commanded: i32,
    /// Speed the measured draw looks like. `None` when there was no reading.
    pub implied: Option<i32>,
    /// Duty measured back off the control pad, as a percentage.
    pub pad_pct: i32,
    pub w_min: f32,
    pub w_mean: f32,
    pub w_max: f32,
    pub on_polls: u32,
    pub off_polls: u32,
    /// Polls where the meter actually served a new number.
    pub fresh: u32,
    pub missed: u32,
    pub flips: u32,
}

impl Window {
    /// True when the meter says the fan is running at a different speed than
    /// the one commanded. The whole reason this tool exists.
    pub fn disagrees(&self) -> bool {
        matches!(self.implied, Some(i) if i != self.commanded)
    }

    /// True when the meter is updating slower than the device polls it, so a
    /// fast cycle could average out into a steady mid-band reading.
    pub fn oversampled(&self) -> bool {
        let read = self.on_polls + self.off_polls;
        read > 0 && self.fresh * 2 < read
    }
}

/// One `auto` line: the whole auto-mode decision, as it was made.
#[derive(Debug, Clone, PartialEq)]
pub struct AutoDecision {
    pub epoch: i64,
    pub inside_f: f32,
    pub outside_f: f32,
    pub delta_f: f32,
    pub latched: bool,
    pub dwell: u32,
    pub dwell_of: u32,
    pub target: i32,
    pub gas: bool,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Record {
    Window(Window),
    Auto(AutoDecision),
    /// `plug CYCLING ...` -- the detector naming the fault.
    CyclingOnset {
        epoch: i64,
        text: String,
    },
    CyclingEnded {
        epoch: i64,
    },
    /// `fan pad MISMATCH ...` -- the chip is not driving what it thinks.
    PadMismatch {
        epoch: i64,
        text: String,
    },
    /// Anything else, kept so it can be counted.
    Other {
        epoch: i64,
        tag: String,
        text: String,
    },
}

impl Record {
    pub fn epoch(&self) -> i64 {
        match self {
            Record::Window(w) => w.epoch,
            Record::Auto(a) => a.epoch,
            Record::CyclingOnset { epoch, .. }
            | Record::CyclingEnded { epoch }
            | Record::PadMismatch { epoch, .. }
            | Record::Other { epoch, .. } => *epoch,
        }
    }

    /// Framework chatter that costs a slot in a 24-line ring and says nothing.
    ///
    /// The one that matters in practice is the Arduino core complaining that a
    /// pin read by `fan::probe_pad` is not configured as a GPIO -- it fires
    /// several times per duty change, which is often enough to push real
    /// telemetry out of the RAM ring before anyone reads it.
    pub fn is_noise(&self) -> bool {
        match self {
            Record::Other { text, .. } => {
                text.contains("is not set as GPIO") || text.contains("may return an in")
            }
            _ => false,
        }
    }
}

/// Value of `key=` up to the next space, if present.
fn field<'a>(text: &'a str, key: &str) -> Option<&'a str> {
    let at = text.find(key)?;
    let rest = &text[at + key.len()..];
    Some(rest.split_whitespace().next().unwrap_or(""))
}

fn num<T: std::str::FromStr>(text: &str, key: &str) -> Option<T> {
    field(text, key)?.trim_end_matches('%').parse().ok()
}

fn flag(text: &str, key: &str) -> bool {
    field(text, key) == Some("on")
}

/// Parse `sp10/~12` into (commanded, implied). `~-1` and `~?` mean no reading.
fn speeds(text: &str) -> Option<(i32, Option<i32>)> {
    let token = text.split_whitespace().find(|t| t.starts_with("sp"))?;
    let (cmd, imp) = token.trim_start_matches("sp").split_once("/~")?;
    let implied = imp.parse::<i32>().ok().filter(|v| *v >= 0);
    Some((cmd.parse().ok()?, implied))
}

/// Parse `w=4.3/12.7/45.3` into (min, mean, max).
fn watts(text: &str) -> Option<(f32, f32, f32)> {
    let mut parts = field(text, "w=")?.split('/');
    Some((
        parts.next()?.parse().ok()?,
        parts.next()?.parse().ok()?,
        parts.next()?.parse().ok()?,
    ))
}

/// Parse `dwell=12/30`.
fn dwell(text: &str) -> Option<(u32, u32)> {
    let (a, b) = field(text, "dwell=")?.split_once('/')?;
    Some((a.parse().ok()?, b.parse().ok()?))
}

/// Parse one line. Returns None for blanks and anything without the two
/// leading timestamps -- a truncated first line after log rotation, say.
pub fn parse_line(line: &str) -> Option<Record> {
    let line = line.trim_end();
    if line.is_empty() {
        return None;
    }
    let mut it = line.splitn(4, ' ');
    let epoch: i64 = it.next()?.parse().ok()?;
    let _uptime: i64 = it.next()?.parse().ok()?;
    let tag = it.next()?.to_string();
    let text = it.next().unwrap_or("").to_string();

    match (tag.as_str(), text.as_str()) {
        ("plug", t) if t.starts_with("window ") => {
            let (commanded, implied) = speeds(t)?;
            let (w_min, w_mean, w_max) = watts(t).unwrap_or((f32::NAN, f32::NAN, f32::NAN));
            Some(Record::Window(Window {
                epoch,
                commanded,
                implied,
                pad_pct: num(t, "pad=").unwrap_or(-1),
                w_min,
                w_mean,
                w_max,
                on_polls: num(t, "on=").unwrap_or(0),
                off_polls: num(t, "off=").unwrap_or(0),
                fresh: num(t, "new=").unwrap_or(0),
                missed: num(t, "miss=").unwrap_or(0),
                flips: num(t, "cyc=").unwrap_or(0),
            }))
        }
        ("plug", t) if t.starts_with("CYCLING") => Some(Record::CyclingOnset {
            epoch,
            text: t.to_string(),
        }),
        ("plug", t) if t.contains("cycling ended") => Some(Record::CyclingEnded { epoch }),
        ("fan", t) if t.contains("pad MISMATCH") => Some(Record::PadMismatch {
            epoch,
            text: t.to_string(),
        }),
        ("auto", t) => Some(Record::Auto(AutoDecision {
            epoch,
            inside_f: num(t, "in=").unwrap_or(f32::NAN),
            outside_f: num(t, "out=").unwrap_or(f32::NAN),
            delta_f: field(t, "d=")
                .and_then(|v| v.trim_start_matches('+').parse().ok())
                .unwrap_or(f32::NAN),
            latched: flag(t, "latch="),
            dwell: dwell(t).map(|d| d.0).unwrap_or(0),
            dwell_of: dwell(t).map(|d| d.1).unwrap_or(0),
            target: num(t, "tgt=").unwrap_or(-1),
            gas: flag(t, "gas="),
        })),
        _ => Some(Record::Other { epoch, tag, text }),
    }
}

/// Parse a whole `/events.log`, oldest first.
pub fn parse(body: &str) -> Vec<Record> {
    body.lines().filter_map(parse_line).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    // Captured from the device on 2026-08-27, not hand-written.
    const WINDOW: &str =
        "1787861886 266426 plug window sp0/~0 pad=0% w=1.3/1.4/1.7 on=0 off=0 new=20 miss=0 cyc=0";
    const AUTO: &str =
        "1787862036 266576 auto in=83.7 out=97.1 d=-13.4 latch=off dwell=0/30 tgt=0 gas=off";
    const NOISE: &str = "1787861886 266426 esp W (266426806) ARDUINO: IO 18 is not set as GPIO. \
                         digitalRead() may return an in";

    fn window(line: &str) -> Window {
        match parse_line(line).unwrap() {
            Record::Window(w) => w,
            other => panic!("not a window: {other:?}"),
        }
    }

    #[test]
    fn it_reads_a_real_window_line() {
        let w = window(WINDOW);
        assert_eq!(w.epoch, 1787861886);
        assert_eq!(w.commanded, 0);
        assert_eq!(w.implied, Some(0));
        assert_eq!(w.pad_pct, 0);
        assert_eq!(w.w_min, 1.3);
        assert_eq!(w.w_max, 1.7);
        assert_eq!(w.fresh, 20);
        assert!(!w.disagrees());
    }

    #[test]
    fn the_night_of_0820_reads_as_a_disagreement() {
        // Commanded 10, drawing the speed-12 level. This is the line the whole
        // tool exists to make impossible to miss.
        let w = window(
            "1787000000 1000 plug window sp10/~12 pad=84% w=4.3/12.7/45.3 \
             on=8 off=28 new=12 miss=0 cyc=5",
        );
        assert_eq!(w.commanded, 10);
        assert_eq!(w.implied, Some(12));
        assert!(w.disagrees());
        assert_eq!(w.flips, 5);
        assert_eq!(w.pad_pct, 84);
    }

    #[test]
    fn a_window_with_no_reading_implies_nothing_rather_than_speed_minus_one() {
        let w = window("1 2 plug window sp10/~-1 pad=84% no meter reads");
        assert_eq!(w.implied, None);
        assert!(!w.disagrees(), "absent must not read as disagreement");
    }

    #[test]
    fn oversampling_is_spotted_from_the_fresh_count() {
        // Few new values across many polls: HA's sensor is slower than our
        // polling, which is exactly when a fast cycle averages into a lie.
        let w = window("1 2 plug window sp10/~10 pad=84% w=1/1/1 on=20 off=16 new=4 miss=0 cyc=0");
        assert!(w.oversampled());
        assert!(
            !window(WINDOW).oversampled(),
            "no polls read is not oversampling"
        );
    }

    #[test]
    fn it_reads_a_real_auto_line_including_a_negative_delta() {
        match parse_line(AUTO).unwrap() {
            Record::Auto(a) => {
                assert_eq!(a.inside_f, 83.7);
                assert_eq!(a.outside_f, 97.1);
                assert_eq!(a.delta_f, -13.4);
                assert!(!a.latched);
                assert_eq!(a.dwell_of, 30);
                assert_eq!(a.target, 0);
            }
            other => panic!("not an auto line: {other:?}"),
        }
    }

    #[test]
    fn a_positive_delta_keeps_its_value_through_the_plus_sign() {
        match parse_line("1 2 auto in=81.0 out=71.6 d=+9.4 latch=on dwell=12/30 tgt=10 gas=off") {
            Some(Record::Auto(a)) => {
                assert_eq!(a.delta_f, 9.4);
                assert!(a.latched);
                assert_eq!(a.dwell, 12);
            }
            other => panic!("not an auto line: {other:?}"),
        }
    }

    #[test]
    fn the_gpio_chatter_is_recognised_as_noise() {
        assert!(parse_line(NOISE).unwrap().is_noise());
        assert!(!parse_line(WINDOW).unwrap().is_noise());
        assert!(!parse_line(AUTO).unwrap().is_noise());
    }

    #[test]
    fn cycling_and_mismatch_lines_get_their_own_kinds() {
        assert!(matches!(
            parse_line("1 2 plug CYCLING 5 flips/10min at speed 10"),
            Some(Record::CyclingOnset { .. })
        ));
        assert!(matches!(
            parse_line("1 2 plug cycling ended after 40min"),
            Some(Record::CyclingEnded { .. })
        ));
        assert!(matches!(
            parse_line("1 2 fan pad MISMATCH want=84% got=0% edges=0 high_us=8400"),
            Some(Record::PadMismatch { .. })
        ));
    }

    #[test]
    fn a_truncated_or_blank_line_is_skipped_not_guessed_at() {
        assert!(parse_line("").is_none());
        assert!(parse_line("   ").is_none());
        assert!(parse_line("not a log line at all").is_none());
        // Rotation can leave a partial first line.
        assert!(parse_line("86 plug window sp0").is_none());
    }

    #[test]
    fn parsing_a_whole_tape_keeps_order_and_drops_only_junk() {
        let body = format!("{WINDOW}\n\n{NOISE}\n{AUTO}\n");
        let recs = parse(&body);
        assert_eq!(recs.len(), 3);
        assert!(recs[0].epoch() <= recs[2].epoch());
    }
}
