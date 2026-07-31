# Codebase Grade Report

**Project:** ESP32-Temp-Sensor
**Audited:** 2026-07-30
**Stack:** ESP32-S2 Arduino/PlatformIO firmware · JSON UI-spec codegen (Python) · Canvas web simulator · pytest + Unity native tests · GitHub Actions CI

Grading draws on a full working session inside this codebase (deep audits by two
subagents with linker-map cross-checks, ~2,000 lines of dead code removed, 15+
bugs fixed and verified on hardware), plus targeted sampling. Evidence is cited
per item.

## Summary

| ID | Category | Grade | Items |
|----|----------|-------|-------|
| A | Architecture & Design | B+ | 3 |
| B | Backend (Firmware) Quality | B | 4 |
| C | Frontend (Web Sim) Quality | C+ | 3 |
| D | Testing & Reliability | C+ | 4 |
| E | Security | B− | 3 |
| F | Dependencies & Tech Currency | C+ | 3 |
| G | Performance & Scalability | B+ | 2 |
| H | Documentation & Onboarding | B | 2 |
| I | Developer Experience & Tooling | B+ | 2 |
| **Overall** | | **B** | **26** |

**Top 5 highest-leverage fixes:** D1, C1, B1, F1, D2

---

## Execution status — updated 2026-07-31

Executed items are struck through in place with commit refs. Verification bar:
firmware items = OTA deploy + on-glass capture; python/tooling = full suite runs.

**Done:** B1(=R2) `4169d27` fw 1.13 · R3 `f7238fc` fw 1.14 (owner's USB plug-in
check still pending) · R1 `0bcc057` fw 1.15 (partial verified via glass capture)
· C1+C2+C3 `c1125ba` · D1 `f8f9168`/`184f32d`/`7954cde` — **726 passed, 0
failed** (root cause: import-time `load_dotenv()` leaking prod broker creds
into the test process; plus mosquitto stdout-pipe deadlock, ms-resolution
client-id collisions, missing pytest-asyncio) · D4+I1+I2 `bbdde7a` (CI now
gates on 142 native tests; `scripts/test-native.sh`, `scripts/deploy.sh`) ·
H1+H2 `1adcd97` (README rewrite, `docs/UI_SPEC.md`).

**Skipped by owner decision (2026-07-30, trusted-LAN posture):** E1, E3, R5, R6.

**Done 2026-07-31 (cont.):** B3+B4+G2 `1493713` fw 1.17 (y-offset sentinel,
NeoPixel rail gating, single boot draw). Outside the audit list: graphs-page
current-reading chips + `frame` op, owner request, `4a75528` fw 1.16.

**Done 2026-07-31 (cont. 2):** A1 `caa4634` (storage_* rename, no aliases) ·
A3+G1 `75f05dd` fw 1.18 (allocation-free render path; spec_render.h pure
header + native_spec_render env, 8 tests — first slice of D2).

**Done 2026-07-31 (cont. 3):** R4 + task #11 `5a9a727` fw 1.19 (outline icon
pipeline: convert_icons.py --outline, 28×28, sim draws the same baked frames).

**Done 2026-07-31 (cont. 4):** E2 `646f387` (optional MQTT_TLS=1, CA from
storage, no passwords — off by default per owner posture) · Tri-color wing
support (Adafruit 4814) `2a93474`: display_hw.h panel selection, `_3c` env,
`color:"red"` spec accent with mono fallback, partial-refresh auto-skip on
the 15 s-flash panel; hardware verify awaits the wing swap (task #25).
fw 1.20 deployed (panel abstraction verified pixel-identical on mono).

**Open:** A2 · B2 · D2 (sparkline-scaling extraction + spec key-coverage
assertion remain) · D3 (history_ring done; storage.cpp CSV parse/rotation
still .cpp-bound) · F1–F3.

Post-execution regrade snapshot (honest read, not re-audited): D C+→**B+**,
C C+→**B**, H B→**A−**, I B+→**A−**; overall B→**B+**. Section grades below
are as originally audited.

---

## A — Architecture & Design — B+

The ui_spec.json → gen_ui.py → firmware + web-sim pipeline is genuinely good
architecture: one source of truth renders on both targets, and this session
proved it catches divergence (the sim caught firmware bugs and vice versa). The
runtime-config overlay (compiled defaults ← device.yaml ← storage JSON, all
bounds-validated in pure headers) is textbook. The historical weakness — modules
built but never wired (net_begin, Logger::begin, HA discovery, boot counters,
all found dead this session) — indicates integration discipline, not design,
and the cure is D1.

#### ~~A1 — Retire the sd_ prefix on the storage layer~~ ✓ done 2026-07-31 (`caa4634`)
- **Where:** `firmware/arduino/src/sd_store.h`, `sd_store.cpp`, call sites in `app_controller.cpp`, `ota_manager.cpp`, `logging/logger.cpp`
- **What's wrong:** The storage layer now runs on internal FFat on the deployed device; every API is still `sd_*`. The header documents the lie, but new code reads misleadingly.
- **Fix:** Rename module to `storage.{h,cpp}` and functions to `storage_*`; keep thin `sd_*` aliases for one release or fix all ~20 call sites in one pass.
- **Effort:** S
- **Grade lift:** B+ → A− (removes the last naming lie in an otherwise honest module map)

#### A2 — Split sim.js (3,809 lines) into modules
- **Where:** `web/sim/sim.js`
- **What's wrong:** Renderer, spec interpreter, validation engine, presets, and metrics export live in one file. Both sim bugs found this session (iconIn rect-name special case, tempMetrics hardcoding) were monolith symptoms.
- **Fix:** Extract `spec-renderer.js` (the drawFromSpec interpreter) and `validation.js` from sim.js; keep globals shim for test compatibility.
- **Effort:** M
- **Grade lift:** B+ → A− (bounded blast radius for future UI work)

#### ~~A3 — spec_format_field: static buffer + String returns~~ ✓ done 2026-07-31 (`75f05dd`, fw 1.18)
- **Where:** `firmware/arduino/src/main.cpp:129-231`
- **What's wrong:** One shared `static char buf[32]` behind heap-allocating String returns on the render path; correctness holds today only because callers consume immediately.
- **Fix:** Take a caller-provided buffer (`bool spec_format_field(const String& key, char* out, size_t n)`), drop String returns.
- **Effort:** M
- **Grade lift:** B+ → A− (removes the render path's last aliasing trap)

---

## B — Backend (Firmware) Quality — B

Strong after this session: storage with graceful FFat fallback and free-space
guarding, MQTT with LWT/rate-limiting/reconnect-hardening, watchdog fed at
every long-blocking boundary, millis-rollover-safe timers throughout (audited
clean by subagent). Docked for the render path's Arduino String usage and the
handful of remaining wide functions.

#### ~~B1 — CSV backfill for the history ring (also roadmap #5)~~ ✓ done 2026-07-30 (`4169d27`, fw 1.13)
- **Where:** `firmware/arduino/src/history_ring.h`, `sd_store.cpp` (reader to add), `app_controller.cpp` boot path
- **What's wrong:** The sparkline ring is RAM-only; every OTA deploy or reboot blanks the graphs page for hours. The data already exists in `/data/YYYY-MM-DD.csv` (inside temp/RH; outside not yet recorded).
- **Fix:** `storage_backfill_ring()`: read today's (+ yesterday's tail) CSV rows into `hist_push`, called after `sd_begin()` in setup. Extend the CSV schema with outside temp/RH columns so future backfills restore both series.
- **Effort:** M
- **Grade lift:** B → B+ (graphs page becomes reboot-proof; completes the feature)

#### B2 — app_controller.cpp is a 900-line god file
- **Where:** `firmware/arduino/src/app_controller.cpp`
- **What's wrong:** Boot orchestration, always-on scheduler, page flipping, pixel effects, diagnostic mode, and sleep phases in one translation unit; app_setup alone spans ~280 lines.
- **Fix:** Extract `always_on_loop.cpp` (scheduler + page/button/twinkle logic) and keep app_controller as boot orchestration only.
- **Effort:** M
- **Grade lift:** B → B+ (the file every feature touches stops growing)

#### ~~B3 — OP_TEXT y-offset overload of p1~~ ✓ done 2026-07-31 (`1493713`)
- **Where:** `firmware/arduino/src/main.cpp` OP_TEXT case; `scripts/gen_ui.py` text emission
- **What's wrong:** `p1` doubles as "explicit y" and "y-offset within rect" with a `!= 0` sentinel; a spec asking for offset 0 silently gets +1.
- **Fix:** Emit a has-offset flag bit in `align`'s upper bits or use INT16_MIN sentinel; mirror in sim.
- **Effort:** S
- **Grade lift:** B → B (small, but removes a spec-authoring footgun before the UI phase leans on it)

#### ~~B4 — NEOPIXEL_POWER never dropped after boot~~ ✓ done 2026-07-31 (`1493713`)
- **Where:** `firmware/arduino/src/diagnostic_test.cpp` (pixel owner), `app_controller.cpp` sleep paths
- **What's wrong:** Subagent finding: GPIO21 stays HIGH forever; the WS2812 idles at ~0.7–1 mA (~3% of the always-on budget). Matters more for deep-sleep builds sharing the code.
- **Fix:** Drop the power rail in `show_boot_stage(0)` when no twinkle is configured, and re-raise lazily in `pixel_flash()` (needs ~1 ms settle).
- **Effort:** S
- **Grade lift:** B → B (battery hygiene; the twinkle feature now makes this a deliberate trade to document either way)

---

## C — Frontend (Web Sim) Quality — C+

The sim is functionally rich (presets, validation engine, layout editor, MQTT
bridge, debug panel) and earned its keep this session as the design surface.
But it's a monolith (A2) with rect-name string special-casing that produced two
real bugs today, plus stale affordances.

#### ~~C1 — Kill remaining rect-name special cases in the spec renderer~~ ✓ done 2026-07-31 (`c1125ba`)
- **Where:** `web/sim/sim.js` drawFromSpec: iconIn "legacy" branch, FOOTER_BATTERY/FOOTER_IP metric exports, INSIDE_TEMP_INNER/BADGE lookups
- **What's wrong:** Behavior keyed on rect *names* rather than op semantics; v3 broke twice on exactly this (double "Cloudy", missing temp metrics). More variants will re-trip it.
- **Fix:** Move per-op behavior onto op fields (e.g. `metricsKey`, `iconOnly`) emitted by gen_ui.py; delete the name-prefix checks and the dead legacy icon+label path.
- **Effort:** M
- **Grade lift:** C+ → B (the next variant costs zero sim surgery)

#### ~~C2 — Sim variant switcher UI~~ ✓ done 2026-07-31 (`c1125ba`)
- **Where:** `web/sim/index.html` controls row; `sim.js` QS handling
- **What's wrong:** Variants are only reachable via `?variant=` URL param or console; the owner iterates on v3/v3g designs but the page has no picker.
- **Fix:** Add a variant `<select>` populated from `UI_SPEC.variants`, wired to redraw; persist in QS.
- **Effort:** S
- **Grade lift:** C+ → B− (the design loop the owner actually uses gets a first-class control)

#### ~~C3 — Dead/duplicate sim entry points~~ ✓ done 2026-07-31 (`c1125ba`)
- **Where:** `web/sim/sim-simple.js`, `geometry_test_dividers.json`, portions of `debug-panel.js`
- **What's wrong:** Alternate renderer and test fixtures with no references from index.html or tests (sampled); they invite the same drift the firmware just recovered from.
- **Fix:** Verify with grep, then delete or move under `web/sim/dev/` with a README line.
- **Effort:** S
- **Grade lift:** C+ → C+ (hygiene)

---

## D — Testing & Reliability — C+

Two honest ledgers: native/UI suites are healthy (137 native tests across 10
envs, all green, added-with-features this session; sim layout/validation suites
green and updated deliberately with design changes) — but `tests/` carries 22
pre-existing failures (broker-dependent integration + snapshot tests), and the
newest firmware surfaces (FFat backend, sparkline renderer, op executor) are
hardware-verified only. A fifth of the python suite failing on a clean run caps
this at C+ regardless of the native health.

#### ~~D1 — Triage the 22 pre-existing python failures [both]~~ ✓ done 2026-07-31 (`f8f9168`, `184f32d`, `7954cde` — 726 passed / 0 failed)
- **Where:** `tests/test_device_manager_async.py` (18), `tests/test_ha_automation_alignment.py`, `tests/test_mqtt_birth.py`, `tests/test_power_hypothesis.py`, `tests/test_snapshot.py`
- **What's wrong:** Most fail on `Subscribe failed rc=4` — they require a live/mock broker that isn't provisioned; the suite can't distinguish regression from environment. This is the fog that hid real bugs before.
- **Fix:** Mark broker-dependent tests with `@pytest.mark.broker` + skip-unless-broker fixture (spin up `mosquitto` in CI where available); fix or re-baseline the hypothesis + snapshot tests individually.
- **Effort:** M
- **Grade lift:** C+ → B (a clean run becomes signal; per repo rule, skips must be tracked to be re-enabled, not abandoned)

#### D2 — Native test env for the op executor's pure logic [BE]
- **Where:** `firmware/arduino/src/main.cpp` (spec_expand_template, spec_format_field routing, sparkline scaling math)
- **What's wrong:** The template expander and sparkline pixel math are pure logic living in an Arduino-bound TU — today's `in -- out --` bug (missing template keys) was exactly the class a native test catches.
- **Fix:** Extract template expansion + sparkline scaling into a pure header (`spec_render_pure.h`) consumed by main.cpp; add `native_spec_render` env with key-coverage tests asserting every field the spec references resolves.
- **Effort:** M
- **Grade lift:** C+ → B (the highest-churn logic gains drift protection)

#### D3 — Storage-layer contract test via host FS [BE]
- **Where:** `firmware/arduino/src/sd_store.cpp` (CSV writer/pruner/log rotation)
- **What's wrong:** Filename/day-parse/rotation logic is tested only by running on hardware; `history_day_from_name` etc. are pure but trapped in the .cpp.
- **Fix:** Move the pure pieces (day parsing, retention decisions, rotation index math) to a header; add native tests. Leave FS calls hardware-only.
- **Effort:** M
- **Grade lift:** C+ → B− (retention bugs become impossible to ship silently)

#### ~~D4 — CI: run the native test envs [BE]~~ ✓ done 2026-07-30 (`bbdde7a`)
- **Where:** `.github/workflows/ci.yml` (runs `pio run` builds; sampled — native `pio test` envs not in the matrix)
- **What's wrong:** 137 native tests exist but CI doesn't execute them; they pass because we run them by hand.
- **Fix:** Add a CI job iterating the `native_*` envs (`pio test -e <env>`), fail-fast.
- **Effort:** S
- **Grade lift:** C+ → B− (the tests become a gate, not a habit)

---

## E — Security — B−

Deliberate, documented posture rather than negligence: open OTA is an explicit
owner decision for a trusted LAN, loudly warned at boot and documented at the
flag (`config.h` OTA_REQUIRE_PASSWORD block). Secrets live in gitignored `.env`
(a commit-time scanner hook enforces hygiene — it ran on every commit today).
MQTT commands are rate-limited; a reboot command exists on an unauthenticated
broker path. Graded on surface, not intent.

#### E1 — MQTT control surface is LAN-open — *skipped, owner decision 2026-07-30 (trusted LAN)*
- **Where:** `firmware/arduino/src/mqtt_client.cpp` cmd handlers (reboot, screenshot, page, sleep_interval, debug)
- **What's wrong:** Anyone who can publish to the broker can reboot the device or pull screenshots. Broker creds gate this today; broker ACLs don't distinguish publishers.
- **Fix:** Per-device broker ACL (mosquitto acl_file: sensor user may publish only under its own base; a separate admin user for cmd/#), or an HMAC nonce on destructive commands.
- **Effort:** M
- **Grade lift:** B− → B+ (control plane matches the trust model instead of inheriting the broker's)

#### ~~E2 — TLS to the broker is unsupported~~ ✓ done 2026-07-31 (`646f387`, compile-time opt-in, off by default)
- **Where:** `mqtt_client.cpp` (WiFiClient, port 1883)
- **What's wrong:** Credentials and commands transit the LAN in cleartext. Acceptable on this network by owner choice; unavailable even as an option.
- **Fix:** Optional `MQTT_TLS=1` path using WiFiClientSecure + CA in FFat; document heap cost (~40 KB).
- **Effort:** M
- **Grade lift:** B− → B (option exists for less-trusted deployments)

#### E3 — Screenshot/log topics leak interior data unauthenticated readers — *skipped, owner decision 2026-07-30 (folds into E1)*
- **Where:** broker retained topics `espsensor/<id>/#`
- **What's wrong:** Retained telemetry (including framebuffer captures) persists for any broker client; same trust-inheritance issue as E1.
- **Fix:** Covered by E1's ACL work; additionally mark screenshot chunks non-retained (they already are) and expire `debug/#` retained values on boot.
- **Effort:** S
- **Grade lift:** B− → B− (folds into E1)

---

## F — Dependencies & Tech Currency — C+

Pinned and reproducible (platformio.ini semver ranges, package-lock present,
venv-pinned python) but aging: Arduino core 2.0.17 (IDF 4.4 line; the 3.x core
is current and changes NeoPixel/RMT + WiFi APIs), PubSubClient 2.8 is
minimally-maintained, GxEPD2 pinned at ^1.5. Nothing EOL-dangerous; upgrade
paths are known-breaking so they rot quietly.

#### F1 — Plan the arduino-esp32 3.x migration — *code-complete 2026-07-31 (`4cb4ab5`, `bda18a2`): pioarduino 55.03.311 / core 3.3.11 / IDF 5.5.5, all envs + TLS variant build warning-free, image fits OTA slots (1216K/1408K), 10 native suites green. Remaining: bench burn-in over USB before any OTA deploy.*
- **Where:** `firmware/arduino/platformio.ini` (espressif32 platform), RMT usage via NeoPixel, WiFi event APIs
- **What's wrong:** Core 2.0.17 is two majors behind; security fixes and S2 improvements land on 3.x only. The longer the wait, the bigger the API delta (subagent verified the legacy RMT path is already in play).
- **Fix:** Spike branch: bump platform, fix compile breaks (NeoPixel/RMT, esp_task_wdt config struct, WiFi events), burn-in on the bench before adopting.
- **Effort:** L
- **Grade lift:** C+ → B (off the deprecated line before it becomes an emergency)

#### F2 — PubSubClient replacement evaluation
- **Where:** `firmware/arduino/src/mqtt_client.cpp`
- **What's wrong:** PubSubClient is effectively frozen (sync-only, 1024-byte ceiling we've already patched around, no MQTT5). Screenshot chunking exists purely to duck its buffer.
- **Fix:** Evaluate espMqttClient (async, fragmented publishes) behind the existing mqtt_* facade; the facade makes this swappable.
- **Effort:** L
- **Grade lift:** C+ → B− (removes the buffer ceiling driving workarounds)

#### F3 — Web sim vendor bundle currency
- **Where:** `web/sim/vendor/mqtt.min.js`, `web/manager/package-lock.json`
- **What's wrong:** Vendored mqtt.js with no version manifest; manager lockfile churned recently but the sim's vendor dir is hand-managed.
- **Fix:** Record versions in a `vendor/VERSIONS.md` or fetch via npm into the manager build.
- **Effort:** S
- **Grade lift:** C+ → C+ (traceability)

---

## G — Performance & Scalability — B+

Strong for the class: the dominant heap-fragmentation source was root-caused to
the 48 Hz OTA UDP poll and fixed (4 Hz, ~99% reduction); thermals engineered
deliberately (80 MHz, modem sleep, 11 dBm) with measured results (38.5→22.5 °C);
e-ink refresh budget explicitly rationed (900 s floor, alternation spends each
refresh on new content); MQTT batched. Remaining items are polish.

#### ~~G1 — Render-path String allocations~~ ✓ done 2026-07-31 (with A3, `75f05dd`)
- **Where:** `main.cpp` spec_expand_template/spec_format_field (per full refresh: dozens of temporary Strings)
- **What's wrong:** Each refresh churns small heap blocks in the 60%-fragmented... now-healthy heap; A3/D2's pure-header refactor removes it for free.
- **Fix:** Same work as A3 — count it once.
- **Effort:** (with A3)
- **Grade lift:** B+ → A− (render path becomes allocation-free)

#### ~~G2 — Boot draws the panel twice~~ ✓ done 2026-07-31 (`1493713`)
- **Where:** `display_manager.cpp:51-60` (init full-white clear) + `run_display_phase()` first real draw
- **What's wrong:** Cold boot spends two full refreshes (~6 s combined) where one would do; also the longest chunk of watchdog budget in setup.
- **Fix:** Skip the init clear when a real draw is guaranteed to follow (flag from app_setup), keeping it for diagnostic builds.
- **Effort:** S
- **Grade lift:** B+ → B+ (faster boot, one refresh cycle saved per reboot)

---

## H — Documentation & Onboarding — B

`docs/ALWAYS_ON_AND_OTA.md` is genuinely good and was kept current with today's
changes (FFat fallback, log mirror, SRAM CS); `hardware/pinmap.md` now records
the SRAM/BUSY findings; the on-card README matches firmware behavior. Gaps: the
root README still centers the deep-sleep story while the flagship build is
always-on two-page v3, and the UI spec pipeline has no authoring guide.

#### ~~H1 — README refresh for the current product~~ ✓ done 2026-07-31 (`1adcd97`)
- **Where:** `README.md`
- **What's wrong:** Describes the pre-v3 world; no mention of the two-page UI, BOOT button, remote screenshot channel, cmd/* MQTT API, or the sim-first design loop. A new contributor would rebuild stale mental models.
- **Fix:** Rewrite the feature section around the always-on build; add the MQTT command table and the sim workflow (`launch.json` sim server, ?variant=).
- **Effort:** S
- **Grade lift:** B → B+ (front door matches the house)

#### ~~H2 — UI spec authoring guide~~ ✓ done 2026-07-31 (`1adcd97`)
- **Where:** new `docs/UI_SPEC.md`
- **What's wrong:** The op vocabulary (fill/inverse, sparkline, tempGroup, guards, variants) lives only in generator source and this session's commits.
- **Fix:** One page: op reference table, rect/variant rules, "add a variant" walkthrough, parity-testing loop.
- **Effort:** S
- **Grade lift:** B → B+ (the pipeline outlives session memory)

---

## I — Developer Experience & Tooling — B+

The loop is genuinely good now: single-source codegen wired into builds, OTA
deploys in ~20 s, a remote framebuffer capture channel (built this session)
closing the verify loop without touching the device, commit-time secret
scanning, clang-format/cpplint/ruff/black in CI, per-module native envs,
launch.json for the sim. Docked for the missing aggregate runners.

#### ~~I1 — `native_all` aggregate test env / make target~~ ✓ done 2026-07-30 (`bbdde7a`, `scripts/test-native.sh`)
- **Where:** `firmware/arduino/platformio.ini`, root `Makefile` (absent)
- **What's wrong:** Running the native suites means 10 separate `pio test -e` invocations (repo memory even notes the absence); friction is why CI never gained them (D4).
- **Fix:** A `scripts/test-native.sh` looping envs (or pio's `-e` multi-flag), called by CI and a `make test` target.
- **Effort:** S
- **Grade lift:** B+ → A− (one command = whole native suite)

#### ~~I2 — Deploy script encapsulating the espota dance~~ ✓ done 2026-07-30 (`bbdde7a`, `scripts/deploy.sh`)
- **Where:** `scripts/` (new `deploy.sh`)
- **What's wrong:** OTA deploys currently require knowing the espota.py path inside the platform package; the ini comment documents a `--project-option` invocation that pio no longer accepts (bit us today).
- **Fix:** `scripts/deploy.sh [ip]`: build always_on, locate espota, upload, tail availability via mosquitto_sub if present. Fix the stale ini comment.
- **Effort:** S
- **Grade lift:** B+ → A− (the most-repeated operation becomes one command)

---

## R — Roadmap Features (owner-requested grading: value vs. effort, and *why*)

Not part of the standard audit; graded A–F on **value for this product**, with
effort and rationale. Execute IDs work here too (e.g. `R1`).

| ID | Feature | Value | Effort | Verdict |
|----|---------|-------|--------|---------|
| R1 | Partial e-ink refresh (SSD1680 + SmartRefresh) | **A** | L | ✓ done fw 1.15 `0bcc057` |
| R2 | CSV backfill of history ring (=B1) | **A−** | M | ✓ done fw 1.13 `4169d27` |
| R3 | USB mass-storage: history as a flash drive | **B+** | M | ✓ done fw 1.14 `f7238fc` (owner plug-in check pending) |
| R4 | Outline icon set unification | **B** | M | ✓ done fw 1.19 `5a9a727` (+ task #11) |
| R5 | STEMMA QT plug-in sensor (CO₂ etc.) | **B−** | S+$ | Skipped (owner) |
| R6 | Capacitive touch on header pads | **C** | M | Skipped (owner) |

#### R1 — Partial e-ink refresh — Value A, Effort L
**Why:** It changes what the product *is*. Today every update costs a ~1 s
full-panel black flash and one of the panel's ~100k refresh cycles, which is
why updates are rationed to 15 min. Partial refresh updates a rectangle in
~300 ms with no flash — temps/clock could track every 5-min sample, the page
flip you found slow gets visibly snappier for small updates, and panel
lifetime stops being the budget. The groundwork already exists and is tested:
SmartRefresh (dirty-region tracking, 15 native tests) and the state layer we
deliberately preserved are exactly this feature's skeleton. Risks: SSD1680
partial-mode ghosting requires a periodic full-refresh "cleanup" cycle
(standard practice), and GxEPD2's paged partial windows need care with the
capture mirror.

#### R2 — CSV backfill (same as B1) — Value A−, Effort M
**Why:** The graphs page you approved is amnesiac: every OTA deploy blanks it
for hours. The fix reads data the device already writes. Cheapest
completion-of-value on the list; also extends the CSV to record outside
readings, which makes the historical record whole.

#### R3 — USB mass-storage mode — Value B+, Effort M
**Why:** Plug the device into any computer and the history mounts as a flash
drive — no MQTT, no scripts, shareable with anyone. The S2 has native USB and
TinyUSB MSC support; FFat is the backing store already. The "wow per hour" is
the best on this list even if utility overlaps with HA history. Risk:
concurrent FS access (firmware writing while host reads) needs a
read-only-while-mounted policy.

#### R4 — Outline icon unification — Value B, Effort M
**Why:** Closes the last sim/device visual divergence (filled blob vs outline)
so the sim is a true preview — which matters more once R1 makes the panel
livelier. Pure polish; bundle with R1's display work.

#### R5 — STEMMA QT sensor — Value B−, Effort S + hardware
**Why:** Zero-solder expansion (CO₂/VOC/PIR/light plug into the spare port);
SCD41 CO₂ would genuinely add new information to the office display. Requires
buying hardware and adds a driver + UI real estate; value depends entirely on
whether you want that data.

#### R6 — Capacitive touch buttons — Value C, Effort M
**Why:** Assembly-dependent (the header pads are sandwiched on your stack),
needs per-unit calibration, and BOOT already covers page-flip well. Revisit
only if a second physical control becomes necessary.

**Recommended order:** R2 (finishes what's shipped) → R1 (the transformation)
→ R4 folded into R1 → R3 as the fun milestone → R5/R6 on demand.
