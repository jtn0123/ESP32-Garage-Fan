//! Read the garage fan's flight recorder and say what the fan is actually doing.
//!
//! Split into a library and a thin binary so the parsing is testable without a
//! device: every format here is one this repo defines, and the samples in the
//! tests are captured from real hardware rather than imagined.

pub mod http;
pub mod report;
pub mod tape;
pub mod trace;
