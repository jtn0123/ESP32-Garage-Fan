//! Turning the records into the answer someone actually wants at 2am.
//!
//! The ordering is deliberate: findings first, context second. A tool that
//! prints a tidy summary and buries "the meter says speed 12 while you asked
//! for 10" three screens down has reproduced the problem it was written to
//! solve -- that was exactly the failure on 2026-08-20, where the console said
//! "doesn't match" and nothing said what it matched instead.

use crate::tape::{Record, Window};
use crate::trace::Trace;

/// How serious a finding is. Ordering matters: `Alarm` sorts first.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Level {
    Alarm,
    Warn,
    Note,
}

impl Level {
    pub fn label(self) -> &'static str {
        match self {
            Level::Alarm => "ALARM",
            Level::Warn => " WARN",
            Level::Note => " note",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Finding {
    pub level: Level,
    pub headline: String,
    /// What to do about it, or what it rules out. Never left empty: a finding
    /// nobody can act on is just a worry.
    pub detail: String,
}

/// Everything the tool concluded, ready to print.
#[derive(Debug, Default)]
pub struct Report {
    pub findings: Vec<Finding>,
    pub lines: Vec<String>,
}

fn f(level: Level, headline: impl Into<String>, detail: impl Into<String>) -> Finding {
    Finding {
        level,
        headline: headline.into(),
        detail: detail.into(),
    }
}

/// Findings from the newest `plug window` line -- the load-bearing check.
fn from_window(w: &Window) -> Vec<Finding> {
    let mut out = Vec::new();

    if w.disagrees() {
        let implied = w.implied.unwrap_or(-1);
        out.push(f(
            Level::Alarm,
            format!(
                "the meter says speed {implied} while the firmware commanded {}",
                w.commanded
            ),
            format!(
                "draw was {:.1}-{:.1} W (mean {:.1}). The pad read back {}%, so the chip {}. \
                 This is the 2026-08-20 signature.",
                w.w_min,
                w.w_max,
                w.w_mean,
                w.pad_pct,
                if w.pad_pct > 0 {
                    "IS driving its waveform -- look at the fan, not the firmware"
                } else {
                    "is NOT driving -- look at the firmware or the wiring"
                }
            ),
        ));
    }

    if w.flips > 0 {
        out.push(f(
            Level::Alarm,
            format!("{} confirmed run/stop flips in the last window", w.flips),
            format!(
                "{} polls looked like running, {} like stopped, at a commanded speed of {}.",
                w.on_polls, w.off_polls, w.commanded
            ),
        ));
    }

    if w.oversampled() {
        out.push(f(
            Level::Warn,
            format!(
                "only {} of {} polls carried a new meter reading",
                w.fresh,
                w.on_polls + w.off_polls
            ),
            "Home Assistant's sensor is updating slower than the device polls it. A cycle \
             faster than that update rate averages into a steady mid-band draw and this tool \
             cannot see it -- the known blind spot.",
        ));
    }

    if w.missed > 0 {
        out.push(f(
            Level::Warn,
            format!("{} polls got no answer from the meter", w.missed),
            "Gaps in the meter feed, not gaps in the fan's behaviour.",
        ));
    }
    out
}

/// Findings from the raw trace, which sees inside the 5-minute windows.
fn from_trace(t: &Trace) -> Vec<Finding> {
    let mut out = Vec::new();
    let flips = t.flips();
    if flips == 0 {
        return out;
    }
    let held = t.speed_held();
    let range = t
        .range()
        .map(|(lo, hi)| format!("{lo:.1}-{hi:.1} W"))
        .unwrap_or_else(|| "no readings".into());
    out.push(f(
        Level::Alarm,
        format!(
            "{flips} run/stop flips across {} minutes of raw samples",
            t.span_s() / 60
        ),
        match held {
            Some(s) => format!(
                "Draw swung {range} while the controller held speed {s} the whole time -- so \
                 the swing is not something the firmware asked for.",
            ),
            None => format!("Draw swung {range}, but the commanded speed also changed here."),
        },
    ));
    out
}

/// The recorder holds 24 lines in RAM. Anything crowding it out is a finding
/// about the instrument itself, which is worth knowing before trusting it.
fn from_noise(records: &[Record]) -> Option<Finding> {
    let total = records.len();
    if total == 0 {
        return None;
    }
    let noise = records.iter().filter(|r| r.is_noise()).count();
    let pct = noise * 100 / total;
    if pct < 20 {
        return None;
    }
    Some(f(
        Level::Warn,
        format!("{pct}% of the tape is framework chatter ({noise} of {total} lines)"),
        "Mostly the Arduino core complaining that a pin read back by fan::probe_pad is not \
         configured as a GPIO. It costs slots in a 24-line RAM ring, so real telemetry can be \
         evicted before anyone reads it.",
    ))
}

/// Build the whole report.
pub fn build(records: &[Record], trace: Option<&Trace>) -> Report {
    let mut report = Report::default();

    let newest_window = records.iter().rev().find_map(|r| match r {
        Record::Window(w) => Some(w),
        _ => None,
    });

    if let Some(w) = newest_window {
        report.findings.extend(from_window(w));
        report.lines.push(format!(
            "last window   commanded speed {}, meter implies {}, pad {}%, {:.1}-{:.1} W",
            w.commanded,
            w.implied
                .map(|i| i.to_string())
                .unwrap_or_else(|| "--".into()),
            w.pad_pct,
            w.w_min,
            w.w_max
        ));
    } else {
        report.findings.push(f(
            Level::Note,
            "no `plug window` line on the tape",
            "Either the firmware predates 1.22.0, or the recorder has rotated past it. \
             Nothing here can speak to what the fan is drawing.",
        ));
    }

    if let Some(a) = records.iter().rev().find_map(|r| match r {
        Record::Auto(a) => Some(a),
        _ => None,
    }) {
        report.lines.push(format!(
            "last decision inside {:.1}F, outside {:.1}F, delta {:+.1}F -> target {} \
             (latch {}, dwell {}/{})",
            a.inside_f,
            a.outside_f,
            a.delta_f,
            a.target,
            if a.latched { "on" } else { "off" },
            a.dwell,
            a.dwell_of
        ));
    }

    for r in records {
        match r {
            Record::CyclingOnset { text, .. } => report.findings.push(f(
                Level::Alarm,
                "the on-device detector called CYCLING",
                text.clone(),
            )),
            Record::PadMismatch { text, .. } => report.findings.push(f(
                Level::Alarm,
                "the control pad is not carrying the duty the firmware set",
                format!("{text} -- this one IS the chip or the wiring, not the fan."),
            )),
            _ => {}
        }
    }

    if let Some(t) = trace {
        report.findings.extend(from_trace(t));
        report.lines.push(format!(
            "raw trace     {} samples at {}s ({} min)",
            t.watts.len(),
            t.poll_s,
            t.span_s() / 60
        ));
    }

    if let Some(n) = from_noise(records) {
        report.findings.push(n);
    }

    report.findings.sort_by_key(|f| f.level);
    report
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::tape;

    fn records(body: &str) -> Vec<Record> {
        tape::parse(body)
    }

    #[test]
    fn a_healthy_tape_raises_nothing() {
        let body = "1 2 plug window sp0/~0 pad=0% w=1.3/1.4/1.7 on=0 off=0 new=20 miss=0 cyc=0\n\
                    3 4 auto in=83.7 out=97.1 d=-13.4 latch=off dwell=0/30 tgt=0 gas=off\n";
        let r = build(&records(body), None);
        assert!(r.findings.is_empty(), "{:?}", r.findings);
        assert_eq!(r.lines.len(), 2);
    }

    #[test]
    fn the_0820_signature_is_the_first_thing_reported() {
        let body = "1 2 plug window sp10/~12 pad=84% w=4.3/12.7/45.3 on=8 off=28 new=36 \
                    miss=0 cyc=5\n";
        let r = build(&records(body), None);
        assert_eq!(r.findings[0].level, Level::Alarm);
        assert!(
            r.findings[0].headline.contains("speed 12"),
            "{:?}",
            r.findings[0]
        );
        assert!(r.findings[0].headline.contains("commanded 10"));
        // And it must say which side to go and look at.
        assert!(r.findings[0].detail.contains("look at the fan"));
    }

    #[test]
    fn a_dead_pad_points_at_the_chip_instead_of_the_fan() {
        let body = "1 2 plug window sp10/~12 pad=0% w=4.3/12.7/45.3 on=8 off=28 new=36 \
                    miss=0 cyc=0\n";
        let r = build(&records(body), None);
        assert!(r.findings[0].detail.contains("look at the firmware"));
    }

    #[test]
    fn a_held_speed_with_a_swinging_draw_says_the_firmware_did_not_ask_for_it() {
        let body = r#"{"poll_s":15,"n":4,"w":[45.1,4.3,45.1,4.3],"spd":[10,10,10,10],
                       "cls":[1,-1,1,-1]}"#;
        let t = crate::trace::parse(body).unwrap();
        let r = build(&[], Some(&t));
        let hit = r
            .findings
            .iter()
            .find(|f| f.headline.contains("flips across"));
        assert!(hit.is_some(), "{:?}", r.findings);
        assert!(hit
            .unwrap()
            .detail
            .contains("not something the firmware asked for"));
    }

    #[test]
    fn an_empty_tape_says_so_rather_than_implying_health() {
        // The dangerous failure: printing nothing and reading as "all clear".
        let r = build(&[], None);
        assert_eq!(r.findings.len(), 1);
        assert!(r.findings[0].headline.contains("no `plug window`"));
    }

    #[test]
    fn framework_chatter_is_reported_once_it_crowds_the_ring() {
        let noise = "1 2 esp W (1) ARDUINO: IO 18 is not set as GPIO. may return an in\n";
        let body = format!(
            "{}1 2 plug window sp0/~0 pad=0% w=1/1/1 on=0 off=0 new=20 miss=0 cyc=0\n",
            noise.repeat(8)
        );
        let r = build(&records(&body), None);
        assert!(
            r.findings
                .iter()
                .any(|f| f.headline.contains("framework chatter")),
            "{:?}",
            r.findings
        );
    }

    #[test]
    fn alarms_sort_above_warnings() {
        let body = "1 2 plug window sp10/~12 pad=84% w=4.3/12.7/45.3 on=20 off=16 new=4 \
                    miss=3 cyc=0\n";
        let r = build(&records(body), None);
        assert_eq!(r.findings[0].level, Level::Alarm);
        assert!(r.findings.windows(2).all(|p| p[0].level <= p[1].level));
    }

    #[test]
    fn every_finding_carries_something_to_do_about_it() {
        let body = "1 2 plug window sp10/~12 pad=84% w=4.3/12.7/45.3 on=20 off=16 new=4 \
                    miss=3 cyc=9\n\
                    3 4 fan pad MISMATCH want=84% got=0% edges=0 high_us=8400\n";
        let r = build(&records(body), None);
        assert!(r.findings.len() >= 4);
        for finding in &r.findings {
            assert!(
                !finding.detail.trim().is_empty(),
                "{finding:?} has no detail"
            );
        }
    }
}
