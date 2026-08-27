# fantape

Read the garage fan's flight recorder and say what the fan is actually doing.

```
fantape 10.0.0.42
```

```
last window   commanded speed 10, meter implies 12, pad 84%, 4.3-45.3 W
last decision inside 81.0F, outside 71.6F, delta +9.4F -> target 10 (latch on, dwell 12/30)
raw trace     60 samples at 15s (15 min)

ALARM  the meter says speed 12 while the firmware commanded 10
        draw was 4.3-45.3 W (mean 12.7). The pad read back 84%, so the chip IS
        driving its waveform -- look at the fan, not the firmware.
ALARM  5 confirmed run/stop flips in the last window
        8 polls looked like running, 28 like stopped, at a commanded speed of 10.
```

## Why it exists

`docs/HARDWARE.md` documents the procedure: fetch `/events.log`, find the
`plug window` lines, compare the commanded speed against the implied one, read
the pad percentage to decide whether the chip or the fan is at fault. It is a
good procedure and a bad thing to be doing by hand at 2am, which is when the
fan does this.

The failure it is aimed at is 2026-08-20, when the controller held speed 10 for
nine hours while the meter showed the speed-12 level in bursts. Nothing said so:
the console reported "doesn't match" without saying what it matched instead.

## Usage

```
fantape <host>              fetch from a device and diagnose
fantape --file <path>       read a saved events.log instead
fantape <host> --no-trace   skip /api/plugtrace (it is token-guarded)
```

Exit codes make it usable from cron or a health check: `0` nothing wrong, `1`
something is wrong with the fan, `2` could not look.

## What it reports

| | |
|---|---|
| **ALARM** | commanded and measured speed disagree; confirmed run/stop flips; the on-device detector called CYCLING; the control pad is not carrying the duty that was set |
| **WARN** | Home Assistant's meter updating slower than we poll it (the known blind spot); polls the meter never answered; framework chatter crowding the 24-line RAM ring |

Every finding carries what to do about it — a test enforces that, because a
finding nobody can act on is just a worry.

## No dependencies

`Cargo.toml` has an empty `[dependencies]`, on purpose. This talks to one
plain-HTTP device on the LAN and parses two formats this repo defines. An HTTP
crate would bring a TLS stack and a few hundred transitive packages to reach
`http://10.x.x.x/events.log`; a JSON crate would bring a derive macro to read
four fixed arrays. Neither buys anything, and both would put a lockfile full of
other people's code in front of a tool whose whole job is to be trustworthy at
2am.

## Developing

```
cargo test           # 37 tests, no device needed
cargo clippy --all-targets -- -D warnings
cargo fmt --check
```

Every sample in the tests is captured from real hardware rather than imagined.
