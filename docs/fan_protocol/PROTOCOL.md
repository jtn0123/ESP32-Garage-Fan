# iLiving ILG8SF12V-DC Fan Control Protocol

Decoded 2026-08-04 by sniffing the wall controller's "fan 2" USB-A port with a
24 MHz fx2lafw logic analyzer (captures in `captures/`, tooling in `analyze.py`).
Bench rig: controller powered 5 V into its "fan 1" port; male USB-A screw-terminal
breakout in "fan 2"; analyzer GND→`−`, CH1→`D+`, CH2→`D−`, CH3→`+`.

## Physical layer

- **Signal wire: USB D+ only.** D− idles high (unused). VBUS on the fan-port side
  reads a weak ~2 V rail (unused for control). The fan normally SOURCES 5 V on
  VBUS to power the controller — never connect a USB host to the fan's cable.
- **Encoding: active-low PWM.** Period **9934 µs (~100.7 Hz)**, extremely stable.
  Fan power is proportional to the LOW fraction of the cycle.
- **Amplitude:** multimeter average (2.785 V at 65% high) implies ~**4.3 V peak**
  (5 V minus a diode drop). Above 3.3 V → ESP32 must transmit through a BSS138
  level shifter and must not receive the line directly on a GPIO.
- **Off = line held solid HIGH** (no pulses at all).
- Pulse widths quantize to ~99.3 µs ticks = 1% of the period; the controller's
  duty table is integer percentages.

## Speed → duty table (measured, 1 s captures, 1 MHz sampling)

| Setting | LOW % (fan on-time) | HIGH width µs | LOW width µs |
|--------:|--------------------:|--------------:|-------------:|
| 0 (off) | 0 — flat HIGH       | —             | 0            |
| 1       | 35.0                | 6457          | 3476         |
| 2       | 41.1                | 5862          | 4073         |
| 3       | 49.2                | 5066          | 4867         |
| 4       | 51.1                | 4868          | 5066         |
| 5       | 56.9                | 4273          | 5662         |
| 6       | 61.9                | 3775          | 6158         |
| 7       | 67.9                | 3180          | 6755         |
| 8       | 72.9                | 2683          | 7253         |
| 9       | 79.0                | 2087          | 7847         |
| 10      | 84.1                | 1590          | 8344         |
| 11      | 89.9                | 994           | 8941         |
| 12      | 95.0                | 497           | 9437         |

Steps are mostly 5–6%, EXCEPT 2→3 (+8.1) and 3→4 (+1.9). Both captures were
internally consistent (repeating for the full second), so this appears to be a
real quirk of the controller's table — worth re-verifying setting 3 once before
freezing firmware. Replicate the measured values verbatim (lookup table, not a
formula).

## Rehearsal result (2026-08-04): PASS

`feather_esp32s2_fan_rehearsal` env (RMT replay on A0/GPIO18 through a BSS138
shifter, HV side captured by the same analyzer) reproduced every table entry
with 0 µs width error (+1 µs on setting 9, rounding). Period measured 9926 µs
vs the controller's 9934 µs — crystal tolerance, irrelevant since the widths
carry the command. Capture: `captures/rehearsal_sweep.sr`. The ESP32 is a
verified drop-in transmitter; next step is fan contact.

## ESP32 replication plan

- LEDC timer at period 9934 µs (~100.66 Hz), inverted output (idle HIGH).
  13-entry lookup table of HIGH-time µs from the table above.
- GPIO → BSS138 LV side; HV side pulled to the fan's own 5 V VBUS; fan D+ on HV.
  Fan VBUS also feeds ESP32 VIN (fan powers the ESP32, as it did the controller).
- Rehearsal before fan contact: transmit into the logic analyzer only and diff
  against these captures until indistinguishable.
- Probe firmware / MQTT actuator work lives on branch
  `claude/mqtt-garage-fan-controller-jcvbc9` (docs/GARAGE_FAN_CONTROLLER.md).
