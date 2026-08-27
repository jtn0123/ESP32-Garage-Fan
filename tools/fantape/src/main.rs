//! `fantape` -- read the garage fan's flight recorder and say what the fan is
//! actually doing.
//!
//! The procedure this replaces is in docs/HARDWARE.md: fetch /events.log, find
//! the `plug window` lines, compare commanded speed against implied speed, read
//! the pad percentage to decide whether the chip or the fan is at fault. That
//! is a good procedure and a bad thing to be doing by hand at 2am.

use std::process::ExitCode;

use fantape::{http, report, tape, trace};

const USAGE: &str = "\
fantape -- read the garage fan's flight recorder

USAGE:
  fantape <host>              fetch from a device and diagnose
  fantape --file <path>       read a saved events.log instead
  fantape <host> --trace-only skip the tape, fetch only the raw samples

OPTIONS:
  --token <t>   token for /api/plugtrace (default: iliving-ota)
  --no-trace    skip /api/plugtrace (it is token-guarded; the tape is not)
  -h, --help    this

EXIT CODE:
  0  nothing wrong found
  1  something is wrong with the fan
  2  could not look
";

/// Exit codes, so this composes into a cron job or a health check rather than
/// only being read by a human.
const OK: u8 = 0;
const FOUND: u8 = 1;
const FAILED: u8 = 2;

struct Args {
    host: Option<String>,
    file: Option<String>,
    token: String,
    trace: bool,
    tape: bool,
}

fn parse_args(argv: &[String]) -> Result<Args, String> {
    let mut a = Args {
        host: None,
        file: None,
        token: "iliving-ota".into(),
        trace: true,
        tape: true,
    };
    let mut it = argv.iter();
    while let Some(arg) = it.next() {
        match arg.as_str() {
            "--file" => a.file = Some(it.next().ok_or("--file needs a path")?.clone()),
            "--token" => a.token = it.next().ok_or("--token needs a value")?.clone(),
            "--no-trace" => a.trace = false,
            "--trace-only" => a.tape = false,
            "-h" | "--help" => return Err(USAGE.into()),
            other if other.starts_with('-') => return Err(format!("unknown option {other}")),
            other => a.host = Some(other.to_string()),
        }
    }
    if a.host.is_none() && a.file.is_none() {
        return Err(USAGE.into());
    }
    Ok(a)
}

fn main() -> ExitCode {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let args = match parse_args(&argv) {
        Ok(a) => a,
        Err(msg) => {
            eprintln!("{msg}");
            return ExitCode::from(FAILED);
        }
    };
    ExitCode::from(run(args))
}

fn run(args: Args) -> u8 {
    // Read the tape first: it is unguarded, so it works even when the token is
    // wrong, and it is the source that answers the main question.
    let body = match (&args.file, &args.host, args.tape) {
        (Some(path), _, _) => match std::fs::read_to_string(path) {
            Ok(b) => Some(b),
            Err(e) => {
                eprintln!("cannot read {path}: {e}");
                return FAILED;
            }
        },
        (None, Some(host), true) => match http::get(host, "/events.log") {
            Ok(b) => Some(b),
            Err(e) => {
                eprintln!("could not read the tape: {e}");
                return FAILED;
            }
        },
        _ => None,
    };
    let records = body.as_deref().map(tape::parse).unwrap_or_default();

    // The trace is token-guarded and optional: a wrong token should cost the
    // extra resolution, not the whole answer.
    let mut traced = None;
    if args.trace {
        if let Some(host) = &args.host {
            let path = format!("/api/plugtrace?token={}", args.token);
            match http::get(host, &path) {
                Ok(b) => traced = trace::parse(&b),
                Err(e) => eprintln!("note: no raw trace ({e})"),
            }
        }
    }

    let report = report::build(&records, traced.as_ref());

    for line in &report.lines {
        println!("{line}");
    }
    if !report.lines.is_empty() {
        println!();
    }

    if report.findings.is_empty() {
        println!("nothing wrong found in {} tape lines.", records.len());
        return OK;
    }
    for finding in &report.findings {
        println!("{}  {}", finding.level.label(), finding.headline);
        println!("        {}", finding.detail);
    }

    let serious = report
        .findings
        .iter()
        .any(|f| f.level == report::Level::Alarm);
    if serious {
        FOUND
    } else {
        OK
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(v: &[&str]) -> Result<Args, String> {
        parse_args(&v.iter().map(|s| s.to_string()).collect::<Vec<_>>())
    }

    #[test]
    fn a_bare_host_is_enough() {
        let a = args(&["10.0.0.42"]).unwrap();
        assert_eq!(a.host.as_deref(), Some("10.0.0.42"));
        assert!(a.trace && a.tape);
    }

    #[test]
    fn a_file_needs_no_host() {
        assert!(args(&["--file", "events.log"]).unwrap().host.is_none());
    }

    #[test]
    fn no_source_at_all_is_refused_rather_than_guessed() {
        assert!(args(&[]).is_err());
    }

    #[test]
    fn an_option_missing_its_value_is_an_error_not_a_silent_default() {
        assert!(args(&["host", "--token"]).is_err());
        assert!(args(&["--file"]).is_err());
    }

    #[test]
    fn an_unknown_option_is_refused_rather_than_read_as_a_host() {
        // Otherwise `fantape --tokne x` would try to resolve "--tokne" as a
        // hostname and report a network problem.
        assert!(args(&["--tokne", "x"]).is_err());
    }
}
