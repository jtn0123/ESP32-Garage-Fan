//! `/api/plugtrace`: the raw 15-second meter samples the tape cannot hold.
//!
//! ```text
//! {"poll_s":15,"n":60,"w":[1.5,1.5,...],"spd":[0,0,...],"cls":[-1,1,...]}
//! ```
//!
//! A fixed shape of four keys, three of them flat number arrays. That is a
//! scanner, not a JSON library -- and a scanner cannot be surprised by a
//! `__proto__` key or a 10 MB string, which is the right posture for something
//! parsing whatever a device on the network hands back.

/// A classification the firmware assigned to one sample.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Class {
    Running,
    Stopped,
    /// Between the bands, or no baseline to judge against.
    Between,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Trace {
    /// Seconds between samples.
    pub poll_s: u32,
    pub watts: Vec<f32>,
    pub speeds: Vec<i32>,
    pub classes: Vec<Class>,
}

impl Trace {
    /// Confirmed run/stop transitions across the whole trace.
    ///
    /// Ignores `Between`, so a sample drifting through the middle band does not
    /// count as two flips -- the same rule net/plug_cycle.h applies on-device.
    pub fn flips(&self) -> u32 {
        let mut flips = 0;
        let mut last: Option<Class> = None;
        for c in self
            .classes
            .iter()
            .copied()
            .filter(|c| *c != Class::Between)
        {
            if last.is_some_and(|l| l != c) {
                flips += 1;
            }
            last = Some(c);
        }
        flips
    }

    /// How long the trace covers, in seconds.
    pub fn span_s(&self) -> u32 {
        self.poll_s * self.watts.len() as u32
    }

    /// (min, max) of the readings, ignoring absent ones.
    pub fn range(&self) -> Option<(f32, f32)> {
        let mut it = self.watts.iter().copied().filter(|w| !w.is_nan());
        let first = it.next()?;
        Some(it.fold((first, first), |(lo, hi), w| (lo.min(w), hi.max(w))))
    }

    /// True when the fan was commanded one speed for the whole trace -- the
    /// case where a swinging draw cannot be explained by the controller.
    pub fn speed_held(&self) -> Option<i32> {
        let first = *self.speeds.first()?;
        self.speeds.iter().all(|s| *s == first).then_some(first)
    }
}

/// Pull the array that follows `"key":[` and parse it with `f`.
fn array<T>(body: &str, key: &str, f: impl Fn(&str) -> Option<T>) -> Vec<T> {
    let needle = format!("\"{key}\":[");
    let Some(start) = body.find(&needle) else {
        return Vec::new();
    };
    let rest = &body[start + needle.len()..];
    let Some(end) = rest.find(']') else {
        return Vec::new();
    };
    // filter_map, so a single unparseable element drops itself rather than
    // truncating the array or taking the whole trace down.
    rest[..end]
        .split(',')
        .map(str::trim)
        .filter(|t| !t.is_empty())
        .filter_map(f)
        .collect()
}

fn scalar(body: &str, key: &str) -> Option<u32> {
    let needle = format!("\"{key}\":");
    let rest = &body[body.find(&needle)? + needle.len()..];
    let end = rest
        .find(|c: char| !c.is_ascii_digit())
        .unwrap_or(rest.len());
    rest[..end].parse().ok()
}

/// Parse a plugtrace body. Returns None when it is not one.
pub fn parse(body: &str) -> Option<Trace> {
    // `null` is how the firmware writes an absent reading; NaN carries that
    // through the rest of the tool as "no number", never as zero.
    let watts = array(body, "w", |t| {
        if t == "null" {
            Some(f32::NAN)
        } else {
            t.parse().ok()
        }
    });
    if watts.is_empty() {
        return None;
    }
    Some(Trace {
        poll_s: scalar(body, "poll_s").unwrap_or(15),
        speeds: array(body, "spd", |t| t.parse().ok()),
        classes: array(body, "cls", |t| {
            t.parse::<i32>().ok().map(|v| match v {
                1 => Class::Running,
                -1 => Class::Stopped,
                _ => Class::Between,
            })
        }),
        watts,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    // Trimmed from the live device, 2026-08-27.
    const REAL: &str =
        r#"{"poll_s":15,"n":4,"w":[1.5,1.5,1.6,1.3],"spd":[0,0,0,0],"cls":[-1,-1,-1,-1]}"#;

    #[test]
    fn it_reads_a_real_trace() {
        let t = parse(REAL).unwrap();
        assert_eq!(t.poll_s, 15);
        assert_eq!(t.watts.len(), 4);
        assert_eq!(t.speeds, vec![0, 0, 0, 0]);
        assert_eq!(t.classes[0], Class::Stopped);
        assert_eq!(t.span_s(), 60);
        assert_eq!(t.speed_held(), Some(0));
        assert_eq!(t.flips(), 0);
    }

    #[test]
    fn the_cycling_signature_counts_its_flips() {
        let body = r#"{"poll_s":15,"n":6,"w":[45.1,4.3,45.1,4.3,45.1,4.3],
                       "spd":[10,10,10,10,10,10],"cls":[1,-1,1,-1,1,-1]}"#;
        let t = parse(body).unwrap();
        assert_eq!(t.flips(), 5);
        assert_eq!(t.speed_held(), Some(10), "the controller never moved");
        let (lo, hi) = t.range().unwrap();
        assert_eq!((lo, hi), (4.3, 45.1));
    }

    #[test]
    fn a_sample_between_the_bands_does_not_manufacture_flips() {
        // Passing through the middle on the way up is one transition, not two.
        let body = r#"{"poll_s":15,"n":3,"w":[4.3,20.0,45.1],"spd":[10,10,10],"cls":[-1,0,1]}"#;
        assert_eq!(parse(body).unwrap().flips(), 1);
    }

    #[test]
    fn a_speed_change_inside_the_trace_is_not_a_held_speed() {
        let body = r#"{"poll_s":15,"n":3,"w":[1,2,3],"spd":[10,10,8],"cls":[0,0,0]}"#;
        assert_eq!(parse(body).unwrap().speed_held(), None);
    }

    #[test]
    fn a_null_reading_stays_absent_rather_than_becoming_zero() {
        // Zero watts is a claim about the fan; null is a claim about the meter.
        let body = r#"{"poll_s":15,"n":3,"w":[1.5,null,1.6],"spd":[0,0,0],"cls":[-1,0,-1]}"#;
        let t = parse(body).unwrap();
        assert!(t.watts[1].is_nan());
        assert_eq!(
            t.range(),
            Some((1.5, 1.6)),
            "the gap must not drag the floor to 0"
        );
    }

    #[test]
    fn something_that_is_not_a_trace_is_refused() {
        assert!(parse("{}").is_none());
        assert!(parse("<html>404</html>").is_none());
        assert!(parse(r#"{"error":"bad token"}"#).is_none());
    }

    #[test]
    fn a_truncated_body_does_not_panic() {
        // The device can drop a socket mid-stream; half a trace must read as
        // no trace rather than take the process down.
        for cut in 1..REAL.len() {
            let _ = parse(&REAL[..cut]);
        }
    }
}
