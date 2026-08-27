//! Just enough HTTP to talk to one device on the LAN.
//!
//! The fan serves plain HTTP from a single-threaded loop on the local network.
//! That is the entire universe this needs to work in, so it is a GET, a
//! `Content-Length` or `Transfer-Encoding: chunked` body, and a timeout --
//! about a hundred lines, against the several hundred transitive packages an
//! HTTP crate would pull in to reach an address that will never be TLS.
//!
//! `/api/plugtrace` is chunked (the firmware streams it through `http_tx`), so
//! chunked decoding is not optional here.

use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::time::Duration;

/// Long enough for an ESP32 that is also serving a page and polling a sensor,
/// short enough that a dead device fails while you are still looking at it.
const TIMEOUT: Duration = Duration::from_secs(10);

/// Refuse a body larger than this rather than grow until the machine hurts.
/// The largest real response is 15 minutes of trace, a few KB.
const MAX_BODY: usize = 8 * 1024 * 1024;

#[derive(Debug)]
pub enum Error {
    Connect(String),
    Io(std::io::Error),
    Status(u16),
    Malformed(&'static str),
    TooLarge,
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::Connect(host) => write!(f, "cannot reach {host}"),
            Error::Io(e) => write!(f, "{e}"),
            Error::Status(401) | Error::Status(403) => {
                write!(f, "refused (403) -- wrong or missing --token")
            }
            Error::Status(404) => write!(f, "not found (404) -- is the firmware current?"),
            Error::Status(c) => write!(f, "device answered {c}"),
            Error::Malformed(what) => write!(f, "malformed response: {what}"),
            Error::TooLarge => write!(f, "response larger than {MAX_BODY} bytes; refusing"),
        }
    }
}

impl From<std::io::Error> for Error {
    fn from(e: std::io::Error) -> Self {
        Error::Io(e)
    }
}

/// GET `path` from `host`, returning the body.
///
/// `host` may carry a port ("10.0.0.5:8080"); port 80 is assumed otherwise.
pub fn get(host: &str, path: &str) -> Result<String, Error> {
    let authority = if host.contains(':') {
        host.to_string()
    } else {
        format!("{host}:80")
    };
    let addr = authority
        .to_socket_addrs()
        .map_err(|_| Error::Connect(host.to_string()))?
        .next()
        .ok_or_else(|| Error::Connect(host.to_string()))?;

    let mut sock =
        TcpStream::connect_timeout(&addr, TIMEOUT).map_err(|_| Error::Connect(host.to_string()))?;
    sock.set_read_timeout(Some(TIMEOUT))?;
    sock.set_write_timeout(Some(TIMEOUT))?;

    // HTTP/1.1 with an explicit close: the firmware's loop serves one request
    // at a time and keep-alive would just leave a socket occupied.
    write!(
        sock,
        "GET {path} HTTP/1.1\r\nHost: {authority}\r\nUser-Agent: fantape\r\nConnection: close\r\nAccept: */*\r\n\r\n"
    )?;
    sock.flush()?;

    read_response(BufReader::new(sock))
}

fn read_response<R: Read>(mut r: BufReader<R>) -> Result<String, Error> {
    let mut status_line = String::new();
    r.read_line(&mut status_line)?;
    let code: u16 = status_line
        .split_whitespace()
        .nth(1)
        .and_then(|c| c.parse().ok())
        .ok_or(Error::Malformed("no status line"))?;

    let mut length: Option<usize> = None;
    let mut chunked = false;
    loop {
        let mut line = String::new();
        if r.read_line(&mut line)? == 0 {
            return Err(Error::Malformed("headers ended early"));
        }
        let line = line.trim_end();
        if line.is_empty() {
            break;
        }
        let (name, value) = match line.split_once(':') {
            Some((n, v)) => (n.trim().to_ascii_lowercase(), v.trim().to_string()),
            None => continue,
        };
        match name.as_str() {
            "content-length" => length = value.parse().ok(),
            "transfer-encoding" if value.eq_ignore_ascii_case("chunked") => chunked = true,
            _ => {}
        }
    }

    let body = if chunked {
        read_chunked(&mut r)?
    } else {
        read_fixed(&mut r, length)?
    };

    // Status is checked after draining so the caller gets a clean socket and,
    // more usefully, so a 403's own explanation is not thrown away.
    if !(200..300).contains(&code) {
        return Err(Error::Status(code));
    }
    Ok(body)
}

fn read_fixed<R: Read>(r: &mut BufReader<R>, length: Option<usize>) -> Result<String, Error> {
    let mut buf = Vec::new();
    match length {
        Some(n) if n > MAX_BODY => return Err(Error::TooLarge),
        // Content-Length present: read exactly that, so a device that holds the
        // socket open does not hang the read.
        Some(n) => r.take(n as u64).read_to_end(&mut buf)?,
        // Absent: the Connection: close above makes EOF the terminator.
        None => r.take(MAX_BODY as u64 + 1).read_to_end(&mut buf)?,
    };
    if buf.len() > MAX_BODY {
        return Err(Error::TooLarge);
    }
    Ok(String::from_utf8_lossy(&buf).into_owned())
}

fn read_chunked<R: Read>(r: &mut BufReader<R>) -> Result<String, Error> {
    let mut out = Vec::new();
    loop {
        let mut header = String::new();
        if r.read_line(&mut header)? == 0 {
            return Err(Error::Malformed("chunk stream ended early"));
        }
        // A chunk size may carry extensions after a ';'.
        let size_hex = header.trim_end().split(';').next().unwrap_or("").trim();
        let size =
            usize::from_str_radix(size_hex, 16).map_err(|_| Error::Malformed("bad chunk size"))?;
        if size == 0 {
            break;
        }
        if out.len() + size > MAX_BODY {
            return Err(Error::TooLarge);
        }
        let mut chunk = vec![0u8; size];
        r.read_exact(&mut chunk)?;
        out.extend_from_slice(&chunk);
        let mut crlf = [0u8; 2];
        r.read_exact(&mut crlf)?;
    }
    Ok(String::from_utf8_lossy(&out).into_owned())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    fn parse(raw: &str) -> Result<String, Error> {
        read_response(BufReader::new(Cursor::new(raw.as_bytes().to_vec())))
    }

    #[test]
    fn it_reads_a_content_length_body() {
        let body = parse("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello").unwrap();
        assert_eq!(body, "hello");
    }

    #[test]
    fn it_reads_a_chunked_body() {
        // /api/plugtrace streams through http_tx::Chunked, so this is the path
        // the most useful endpoint actually takes.
        let raw = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n\
                   4\r\n{\"n\"\r\n3\r\n:1}\r\n0\r\n\r\n";
        assert_eq!(parse(raw).unwrap(), "{\"n\":1}");
    }

    #[test]
    fn a_chunk_extension_does_not_derail_the_size() {
        let raw = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n\
                   5;foo=bar\r\nhello\r\n0\r\n\r\n";
        assert_eq!(parse(raw).unwrap(), "hello");
    }

    #[test]
    fn a_bad_token_reads_as_a_token_problem_not_a_parse_problem() {
        let e = parse("HTTP/1.1 403 Forbidden\r\nContent-Length: 2\r\n\r\n{}").unwrap_err();
        assert!(e.to_string().contains("--token"), "{e}");
    }

    #[test]
    fn a_body_with_no_length_ends_at_eof() {
        assert_eq!(
            parse("HTTP/1.1 200 OK\r\n\r\ntrailing").unwrap(),
            "trailing"
        );
    }

    #[test]
    fn a_truncated_chunk_stream_is_an_error_not_a_silent_short_read() {
        // The failure that matters: half a trace read as a whole one would
        // under-report flips and make a cycling fan look calm.
        let raw = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel";
        assert!(parse(raw).is_err());
    }

    #[test]
    fn headers_are_matched_case_insensitively() {
        assert_eq!(
            parse("HTTP/1.1 200 OK\r\nCONTENT-LENGTH: 2\r\n\r\nhi").unwrap(),
            "hi"
        );
    }
}
