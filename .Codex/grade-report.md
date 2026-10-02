# Codebase Grade Report

**Project:** ESP32-Garage-Fan
**Audited:** 2026-09-29
**Stack:** ESP32-S2 Arduino C++, TypeScript/Bun embedded console, Python host tooling, Rust diagnostics.

## Summary

| ID | Category | Grade | Items |
|----|----------|-------|-------|
| A | Architecture & Design | B+ | 1 |
| B | Backend Quality | C+ | 4 |
| C | Frontend Quality | B− | 3 |
| D | Testing & Reliability | B− | 3 |
| E | Security | C+ | 2 |
| F | Dependencies & Tech Currency | C+ | 2 |
| G | Performance & Scalability | C+ | 2 |
| H | Documentation & Onboarding | B− | 2 |
| I | Developer Experience & Tooling | B− | 1 |
| **Overall** | | **C+** | **20** |

**Completed priorities:** E1, B1, B2, C1, D1, A1, E2, D2, I1, F1 (10 of 20 items).

Grades above are the original audit baseline. Findings below preserve the pre-fix evidence; executed items are marked done. Regrade the affected categories after reviewing these changes.

Overall weights the concrete credential/control defects and insecure defaults more heavily than the strong module structure and extensive console suite. This is a sampled engineering audit, not physical validation, a penetration test or proof of full remote CI/Sonar health. The requested top 10 fixes are now implemented and locally verified; Uptime Kuma work is preserved. Firmware 1.27.0 has now been deployed and validated on the controller. No physical fan failure was induced.

## Validation and integration evidence

**Completed-fix validation (2026-09-29):**

- Firmware controller build passed without source warnings; the embedded gzip console matches `web/dist/console.html` exactly.
- 165 Python tests passed, including 17 actual C++ handler cases. Seven handler regressions reproduce against the original code before the fixes.
- `make web` passed from a fresh frozen Bun installation: TypeScript, 138 Vitest tests and 253 desktop/mobile browser tests. A desktop hover race exposed by live SSE repainting was fixed; 40 repeated affected tests and the final full 253-test suite passed.
- All 22 discovered native environments passed (241 cases); 39 Rust tests passed.
- Ruff, Black, strict mypy, changed C++ cpplint/formatting and shell syntax checks passed. `bun audit` reports zero vulnerabilities.
- Authentication is now header-based. Older firmware requires its legacy updater or USB for the first upgrade; the former public query token supported a one-time OTA carrying a private-token image. Private credentials never went into URLs. Rolling back before the credential-snapshot format uses legacy credentials.
- Build/browser temporary storage moved to `/Volumes/512Flash/garage-fan-validation-20260929` after an internal-disk ENOSPC failure. No unrelated files were removed.
- PR #69 is pushed; remote build, test, lint, security, CodeQL and Sonar checks pass. Both controller and rehearsal images build.
- On 2026-09-30, the verified 1,263,120-byte ESP32-S2 image was installed in ota_0, confirmed through MQTT, and returned HTTP 200 from `/health` once weather/power dependencies warmed up. SHA256: `9c7837315a79844761b9546d960e3d95b653f9ae7f246b648ca2f492169e3ca7`. The first Python upload aborted after 119,188 bytes with the old image intact; standard curl multipart completed in 23.9 seconds.
- Twenty-nine live API checks passed: reads/history/CSV/display, actual pad probe, private header authorization, public/query-token rejection, malformed numbers/late credential fields, atomic configuration rejection, cross-origin and GET-write rejection, and safe same-speed control. Unchanged credential provisioning and another reboot preserved the network, private token and controller configuration.
- Live desktop (1280 px) and mobile (390 px) console DOM checks passed with no horizontal overflow; settings and display mirror loaded, password fields stayed blank, and auto controls changed real state and restored auto mode/speed 10. Shared-preview screenshot capture failed, so this is DOM/runtime evidence, not screenshot review.
- Kuma monitor #30 now uses HTTP `/health`, accepted code 200 only, a 60-second interval, two retries and a 20-second timeout. Existing history and group remain intact; fresh server heartbeats return 200 - OK. No notification channel is configured.
- These checks establish controller readiness and electrical output/power agreement, not tachometer or blade-rotation proof. Storage/driver failures are exercised in host tests, not induced on the live card or PWM hardware.

**Original audit/integration baseline:**

- Baseline: 123 Python tests, 132 Vitest tests, TypeScript check and generated console build passed; Rust tests passed (37).
- Updated health contract: 134 Python tests passed. Ruff, Black, strict mypy and new firmware cpplint passed.
- Native runner: 20 discovered environments passed. Two digit-bearing environments were omitted by the runner (D1); explicit execution of both and the final health suite passed (12 tests).
- ESP32-S2 controller firmware compiled successfully. New `/health` is local build output, not yet deployed; live device remains 1.26.0.
- Browser suite initially could not launch because Chromium v1234 was absent. Matching Chromium was installed; all 249 desktop/mobile browser tests then passed (2.2 minutes).
- bun audit: 12 development-tool advisories, described under F1. Python/firmware CVE coverage was not independently completed.
- Live Kuma monitor #30 created in Devices & network and verified Up across multiple 60-second checks using `/api/state`. It checks MQTT, confirmation, weather in automatic mode, and optional power-meter freshness/disagreement/cycling. No notification channel exists. Indoor freshness requires deployment of the new `/health` endpoint.
- Supplied API key verified HTTP 200 on Kuma `/metrics`; the key was not stored in source, report, firmware or browser assets. Monitor creation used the user-authenticated shared preview.
- A first firmware build failed with missing build directories while native PlatformIO tests were running in the same tree; serialized controller rebuild succeeded. Treat PlatformIO operations sharing this build tree as sequential.

---

## A — Architecture & Design — B+

Modules own their state behind narrow APIs, and the orchestrator remains separate from HTTP handlers (`firmware/arduino/src/fan_controller_main.cpp:201-294`, `src/net/web.cpp`). Pure decision headers make embedded behavior testable. The actuator boundary still declares success without knowing whether hardware accepted the command.

#### ~~A1~~ ✓ done 2026-09-29 — Propagate actuator failures before reporting a new speed
- **Completion:** PWM attach/write failures return failure before saving or announcing speed. Failed requests return 503, retain the accepted command and allow retry; boot output can report unknown. State, health and console expose the driver fault. Injected attach/write and browser fault regressions pass; no physical failure was induced.
- **Where:** firmware/arduino/src/fan/control.cpp:59-79,126-145
- **What's wrong:** set_wave returns void when LEDC attach/write fails, while apply has already assigned the new speed, persists it and publishes it. Identical subsequent requests short-circuit. This is a confirmed code failure path; no hardware failure was induced.
- **Impact:** Major — displayed and retained speed can disagree with the actual output.
- **Fix:** Return a success result from the actuator operation; update commanded state only after success, expose the failure, allow retry, and test an injected driver failure.
- **Effort:** M
- **Grade lift:** B+ → A− — explicit actuator success/failure semantics.

---

## B — Backend Quality — C+

HTTP modules bound their outputs, stream bulk data, and handle interrupted OTA uploads (`firmware/arduino/src/net/web_history.cpp:285-325`, `web_ota.cpp:106-140`). However, provisioning is not atomic and numeric parsing converts malformed commands into valid control actions. The supported absent-barometer state also produces invalid retained MQTT JSON.

#### ~~B1~~ ✓ done 2026-09-29 — Validate provisioning completely before persistence
- **Completion:** Staged all supplied credentials and saved one versioned NVS blob with checked persistence. Legacy stores migrate; validation/write failures keep the previous set and do not reboot. Production-handler regressions cover invalid late fields, storage failure, restart persistence and migration.
- **Where:** firmware/arduino/src/net/web_provision.cpp:33-75; firmware/arduino/src/net/creds.cpp:93-117
- **What's wrong:** The preflight checks only empty SSIDs and a permissively parsed port. Later length validation happens during sequential NVS writes, so a valid new SSID followed by an overlong password returns 400 after saving the SSID; 1883junk similarly passes preflight before later rejection.
- **Impact:** Major — a rejected settings request can leave partial credentials and disconnect the device on its next reboot.
- **Fix:** Share side-effect-free validation with set_field, stage and validate the full request first, then persist it with checked writes and defined rollback behavior. Add invalid-late-field cases.
- **Effort:** M
- **Grade lift:** C+ → B− — restores all-or-nothing credential updates.

#### ~~B2~~ ✓ done 2026-09-29 — Reject malformed control numbers
- **Completion:** Strict complete-input integer and finite-float parsing now guards speed, raw duty, provisioning port and every config field before mutation. Actual production handlers and the mock reject malformed/out-of-range requests without partial changes.
- **Where:** firmware/arduino/src/net/web.cpp:204-247,298-322
- **What's wrong:** String::toInt and toFloat accept numeric prefixes and turn nonnumeric values into zero. speed=abc therefore requests fan off; malformed settings can also become accepted zero-valued controls. The MQTT path already uses a stricter end-pointer check. This was identified in source, not sent to the live fan.
- **Impact:** Major — an invalid client request can stop the fan or disable automatic control.
- **Fix:** Use strict complete-input integer/finite-float parsers, validate all supplied configuration fields before applying any, and return HTTP 400 naming the invalid field.
- **Effort:** S
- **Grade lift:** C+ → B− — errors stop becoming control actions.

#### B3 — Emit JSON null for missing pressure in MQTT
- **Where:** firmware/arduino/src/net/mqtt_link.cpp:91-96; firmware/arduino/src/sensors/climate.cpp:62-75
- **What's wrong:** Climate sampling deliberately succeeds with NAN pressure when the barometer is unavailable, but publish_climate formats pressure with %.1f, yielding hpa:nan. The identical formatter output was locally rejected by a strict JSON decoder.
- **Impact:** Moderate — consumers lose otherwise valid temperature/humidity telemetry when an optional barometer fails.
- **Fix:** Serialize nonfinite pressure as null, matching /api/sensors, and add a strict JSON-decoding test of the production formatter.
- **Effort:** S
- **Grade lift:** C+ → B− — optional sensor failure preserves valid telemetry.

#### B4 — Escape JSON control characters in identity strings
- **Where:** firmware/arduino/src/net/web.cpp:154-160; firmware/arduino/src/net/creds.cpp:93-117
- **What's wrong:** json_str escapes quotes and backslashes but leaves control characters such as a newline untouched. Those are accepted by credential storage and make the device JSON invalid; a local strict-decoder reproduction confirmed it.
- **Impact:** Moderate — an unusual but accepted SSID or identity field can break provisioning/settings reads.
- **Fix:** Use a bounded shared encoder that emits control characters as unicode escapes, checks truncation, and test newline/tab/quote/backslash inputs.
- **Effort:** S
- **Grade lift:** C+ → B− — stored strings remain valid on the wire.

---

## C — Frontend Quality — B−

The console separates state, rendering and orchestration and protects history against late responses (`web/src/state.ts:12-64`, `app.ts:233-278`). Labeled settings controls and desktop/mobile interaction coverage are substantive strengths. Password handling, failed command feedback and keyboard interactions still have concrete gaps.

#### ~~C1~~ ✓ done 2026-09-29 — Preserve password whitespace during provisioning
- **Completion:** Both passwords preserve whitespace through serialization, POST body transport and firmware persistence. Unit, browser and actual-handler regressions pass.
- **Where:** web/src/provision.ts:53-63; web/tests/provision.test.ts:38-49
- **What's wrong:** The shared serializer trims every field, including WiFi and MQTT passwords. Direct execution with the dummy password " example " transmitted "example". Firmware preserves the value it receives.
- **Impact:** Major — a valid password containing surrounding spaces becomes incorrect and can leave the controller unreachable after reboot.
- **Fix:** Preserve both password fields exactly; normalize only appropriate host/numeric fields. Add regression cases for leading and trailing spaces.
- **Effort:** S
- **Grade lift:** B− → B — removes a demonstrated provisioning failure.

#### C2 — Display refused or failed control commands
- **Where:** web/src/app.ts:89-99,359-361
- **What's wrong:** command catches speed/config request failures silently. Polling can keep working without explaining why a requested control change failed.
- **Impact:** Moderate — fan-off, speed and automatic-mode actions can appear unresponsive and invite repeated attempts.
- **Fix:** Show an accessible error beside the controls, clear it after success, and add a browser case rejecting the POST while reads still succeed.
- **Effort:** S
- **Grade lift:** B− → B — makes command outcomes visible.

#### C3 — Make waveform and status explanations keyboard accessible
- **Where:** web/src/body.html:42-50; web/src/app.ts:362-366; web/src/status_bits.ts:120-140
- **What's wrong:** The waveform opener is a clickable div and status explanations are clickable spans without focus or keyboard activation semantics.
- **Impact:** Moderate — keyboard users cannot open these explanations.
- **Fix:** Use styled buttons, maintain expanded/controls attributes, and add keyboard activation and dismissal browser tests.
- **Effort:** S
- **Grade lift:** B− → B — extends semantic controls across the console.

---

## D — Testing & Reliability — B−

The project has real source-based Unity tests, wire-contract checks, 132 browser unit tests, a 249-case desktop/mobile browser suite and 37 Rust tests (`firmware/arduino/test`, `tests`, `web/e2e`, `tools/fantape`). However, native test discovery silently omits two environments and HTTP tests exercise a specification mock rather than the firmware parser. Raw script coverage measured 18% on the baseline but does not capture subprocess execution, so it is not an accurate whole-system coverage claim.

#### ~~D1~~ ✓ done 2026-09-29 — [BE] Discover native environment names containing digits
- **Completion:** Native discovery accepts digits and is checked against every INI declaration. The repaired runner passed all 22 environments, including native_base64_stream and native_qr_v1 (241 cases total).
- **Where:** scripts/test-native.sh:8; firmware/arduino/platformio.ini:224-238; .github/workflows/ci.yml:248-250
- **What's wrong:** The discovery regex allows only letters and underscores. native_base64_stream and native_qr_v1 are defined but silently excluded from make test and CI. The runner completed 20 environments including the new health suite; these two required explicit invocation.
- **Impact:** Major — CI can stay green while two shipped firmware algorithms have no execution coverage.
- **Fix:** Allow digits in environment names or parse the INI structurally; test discovered names against every native environment declaration.
- **Effort:** S
- **Grade lift:** B− → B — every declared native suite runs.

#### ~~D2~~ ✓ done 2026-09-29 — [BE] Exercise production handler validation instead of only the mock
- **Completion:** Added a C++ host harness compiling real web_controls, web_provision, creds and fan/control sources with fallible platform stubs. Fifteen handler cases pass. Seven regressions fail against the original HEAD implementation and pass after fixes; existing mock/browser coverage is retained.
- **Where:** tests/test_http_contract.py:1-7; scripts/mock_device.py:273-310; firmware/arduino/src/net/web.cpp:317-322
- **What's wrong:** The mock strictly rejects speed=abc, so its HTTP regression passes while firmware converts abc to speed zero. The same mock boundary cannot catch partial NVS updates.
- **Impact:** Major — passing HTTP tests can miss real controller behavior defects.
- **Fix:** Extract production parsing and request validation into pure headers exercised natively, and add a lightweight actual-handler integration harness or opt-in device contract check. Do not replace mock UI tests.
- **Effort:** M
- **Grade lift:** B− → B — contract assertions cover the implementation that ships.

#### D3 — [BE] Measure subprocess coverage and cover host-tool failures
- **Where:** tests/test_gen_device_header_wifi.py; scripts/gen_device_header.py; scripts/flash.py; .github/workflows/ci.yml:299-318
- **What's wrong:** Baseline pytest script coverage reports only 18%, including 0% for flash.py and 21% for header generation. Some generator/mock execution happens in subprocesses and is not measured; flash tooling has no direct regression suite under tests.
- **Impact:** Moderate — coverage reports cannot distinguish unmeasured execution from untested deployment failure paths.
- **Fix:** Enable subprocess coverage or test the real generator functions directly; add credential/build/upload failure cases around flash.py with mocked process and serial boundaries. Record accurate scope and add a realistic ratchet.
- **Effort:** M
- **Grade lift:** B− → B — meaningful measurement and host-operation failure coverage.

---

## E — Security — C+

Central origin gates protect browser-driven writes and privileged routes reject empty tokens (`firmware/arduino/src/net/web_gate.h:18-72`). Secret files and generated credential headers are ignored, and provisioning logs field names rather than values. A public privileged-token fallback and secret-bearing URLs remain; the intentional unauthenticated trusted-LAN control policy is documented. The live token was not inspected.

#### ~~E1~~ ✓ done 2026-09-29 — Remove the public privileged-token fallback
- **Completion:** Removed privileged defaults from firmware, console and tools. Public, placeholder, invalid and missing tokens fail closed; valid private NVS tokens survive release updates. Generator, handler and blank-token updater regressions pass.
- **Where:** firmware/arduino/src/config.h:55-56; firmware/arduino/src/net/web.cpp:328-331; scripts/gen_device_header.py:345-356; scripts/deploy.sh:24-34
- **What's wrong:** Builds without a configured/NVS token use a publicly committed privileged token. Generation allows it and deployment only warns. This establishes an insecure default, not that the live controller uses it.
- **Impact:** Major — any reachable controller using the default allows privileged operations to someone who read the repository.
- **Fix:** Fail closed with an empty default or persist a unique token during provisioning; refuse the public fallback during deployment except for an explicit bench-only override.
- **Effort:** M
- **Grade lift:** C+ → B− — normal builds no longer share public privileged credentials.

#### ~~E2~~ ✓ done 2026-09-29 — Keep passwords and authorization tokens out of URLs
- **Completion:** Privileged clients use X-Fan-Token; provisioning and token rotation use POST bodies. Firmware, mock, console, deploy script and fantape agree. Deploy headers use a temporary private file deleted on exit. Header/body assertions, legacy-query rejection and Rust header-injection tests pass.
- **Where:** web/src/provision.ts:53-63; web/src/api.ts:68-75,84-106; firmware/arduino/src/net/web_gate.h:25-29
- **What's wrong:** WiFi/MQTT passwords and privileged-operation tokens travel in query parameters even on POST requests. URLs are routinely captured by request diagnostics and infrastructure logs; no actual exposure was inspected here.
- **Impact:** Major — incidental URL logging can disclose network or controller credentials.
- **Fix:** Send provisioning fields in a POST body and authorization in a collected header; update firmware, deployment script, mock and browser tests. Keep the trusted-LAN HTTP boundary documented.
- **Effort:** M
- **Grade lift:** C+ → B− — removes avoidable secret exposure from request URLs.

---

## F — Dependencies & Tech Currency — C+

The web toolchain has a committed Bun lock, firmware platform releases are pinned, and Dependabot covers web, Python, Rust and workflow dependencies (`web/bun.lock`, `firmware/arduino/platformio.ini:10`, `.github/dependabot.yml`). A live bun audit reports 12 advisories in two development dependency packages. Python and firmware library resolution remains less reproducible than the web/Rust setup.

#### ~~F1~~ ✓ done 2026-09-29 — Update vulnerable development dependency resolutions
- **Completion:** Compatible dev-tool overrides resolve nanoid 3.3.19 and undici 8.11.2. Fresh frozen installation and bun audit pass with zero vulnerabilities; console and browser checks pass.
- **Where:** web/bun.lock:250,304; web/package.json:20-26
- **What's wrong:** bun audit reported 12 vulnerabilities (4 high, 5 moderate, 3 low): nanoid 3.3.17 via Vitest/Vite/PostCSS and undici 8.10.0 via jsdom. These are development tooling, not evidence of vulnerabilities executing on the ESP32. Primary advisory sources: https://github.com/advisories/GHSA-2v37-7h3g-55p8 and https://github.com/advisories/GHSA-w293-vg96-wgc3.
- **Impact:** Moderate — known vulnerable tooling stays installed in developer/CI environments.
- **Fix:** Resolve nanoid to at least 3.3.18 and undici to at least 8.10.2 through compatible updates; inspect the lock diff, rerun audit, typecheck, unit/browser tests and verify identical generated console bytes where expected.
- **Effort:** S
- **Grade lift:** C+ → B− — removes confirmed vulnerable resolutions.

#### F2 — Make Python and firmware dependency resolution reproducible
- **Where:** requirements.txt:1-4; requirements-dev.txt; requirements-firmware.txt; firmware/arduino/platformio.ini:15-28; .github/workflows/ci.yml:363-386
- **What's wrong:** Python requirement ranges and unpinned formatter installs can resolve differently across runs. Firmware platform is pinned, but caret library ranges are not locked. Local requirements-dev requests newer mypy than CI pins.
- **Impact:** Moderate — a fresh checkout can test or build with a different tool/library set from CI or a prior release.
- **Fix:** Keep updateable input manifests while generating pinned/hash-checked Python constraints; record exact firmware library resolutions for releases; align local and CI lint versions.
- **Effort:** M
- **Grade lift:** C+ → B− — releases and tooling have repeatable inputs.

---

## G — Performance & Scalability — C+

PSRAM history, streamed responses, blocked-peer removal and asynchronous panel refresh are appropriate for a small controller (`firmware/arduino/src/storage/history.cpp:14-35`, `net/sse.cpp:22-44`, `ui/display.cpp:58-67`). Outbound waits and downloads still block the loop serving control/sensors. Kuma live checks showed response times from roughly 174 ms to several seconds; the exact cause of that live variation was not isolated.

#### G1 — Bound outbound waits outside the shared control loop
- **Where:** firmware/arduino/src/net/mqtt_link.cpp:117-125; firmware/arduino/src/net/weather.cpp:43-77; firmware/arduino/src/net/plug.cpp:191-215; firmware/arduino/src/fan_controller_main.cpp:230-234,279-287
- **What's wrong:** MQTT reconnect uses its synchronous default socket timeout; weather reads can wait ten seconds and plug requests five seconds, plus connection/DNS waits. These execute in the shared loop. Watchdog resets do not preserve responsiveness during the waits.
- **Impact:** Moderate — failing services can pause web requests and automatic-control/sensor work for seconds.
- **Fix:** Set explicit connection/read deadlines and MQTT socket timeouts, use retry backoff, and isolate polling in a bounded worker or incremental state machine. Measure loop latency during forced remote failures.
- **Effort:** M
- **Grade lift:** C+ → B− — remote failures stop monopolizing device work.

#### G2 — Apply an overall budget to streamed responses
- **Where:** firmware/arduino/src/net/http_tx.cpp:23-48,73-79; firmware/arduino/src/net/web_history.cpp:343-359
- **What's wrong:** The transport deadline restarts after progress and every chunk receives a fresh deadline. A slow client making continuous small progress can hold the synchronous CSV handler without a response-wide time limit. This is code evidence; no slow-client device experiment was run.
- **Impact:** Moderate — a bulk download can postpone readings, control work and other HTTP clients indefinitely.
- **Fix:** Carry a shared overall response deadline/byte-time budget across chunks; abort at the limit or resume bulk transfers incrementally. Test a sink that never fully stalls but progresses too slowly.
- **Effort:** M
- **Grade lift:** C+ → B− — bounds total handler occupancy.

---

## H — Documentation & Onboarding — B−

README explains the electrical hazard, API method policy, credentials and release behavior, while the hardware/protocol documents contain measured evidence (`README.md`, `docs/HARDWARE.md`, `docs/fan_protocol/PROTOCOL.md`). The new Kuma instructions document both live state monitoring and the deployment-only health endpoint. Existing product descriptions and prerequisites still contradict the actual implementation.

#### H1 — Reconcile product descriptions with current hardware and updater
- **Where:** README.md:21-23,36-37; README.md final Lineage section; firmware/arduino/src/fan/control.cpp:61-76; docs/HARDWARE.md:261
- **What's wrong:** README credits RMT with output continuity, while firmware uses LEDC and hardware notes explain why. It says display firmware was removed although this controller drives a display; updater text describes only a binary link although checksum-verified installation exists.
- **Impact:** Moderate — maintainers receive conflicting descriptions of waveform ownership, display hardware and OTA capabilities.
- **Fix:** Describe LEDC, distinguish the removed inherited display stack from the active fan display, and describe the current verified updater behavior.
- **Effort:** S
- **Grade lift:** B− → B — operational descriptions agree with code.

#### H2 — Document complete prerequisites for a fresh checkout
- **Where:** README.md Build section; requirements*.txt; web/package.json:6-17; Makefile:35-67
- **What's wrong:** Build examples assume PlatformIO, Python tooling, Bun and browser binaries are installed. README says make web needs Node, although scripts invoke Bun; the npm-based target failure is owned by I1. This run also needed the pinned Chromium binary installed before browser tests could execute.
- **Impact:** Moderate — a new checkout has no complete sequence leading to a working build and check.
- **Fix:** Document supported Python/Bun versions, firmware/dev requirement installation, bun frozen setup and Chromium installation; reference the repaired I1 target and optional Rust checks.
- **Effort:** S
- **Grade lift:** B− → B — onboarding gives executable prerequisites.

---

## I — Developer Experience & Tooling — B−

CI separates firmware, browser, Python, Rust, warning and lint gates, and strict Python/TypeScript checks are configured (`.github/workflows/ci.yml`, `pyproject.toml`, `web/tsconfig.json`). Generated console-byte freshness is checked. The advertised local web entrypoint currently fails before it reaches those checks.

#### ~~I1~~ ✓ done 2026-09-29 — Repair make web to use the committed package manager
- **Completion:** make web now uses frozen Bun installation and the existing complete check command. A fresh dependency installation passed TypeScript, 138 unit tests, bundle generation and all 253 browser tests; setup prerequisites are documented.
- **Where:** Makefile:58-61; web/package.json:6-17; web/bun.lock; README.md Build section
- **What's wrong:** make web executes npm ci but no package-lock is committed; only bun.lock exists, and package scripts themselves invoke Bun. npm ci --dry-run --ignore-scripts reproduced exit 1/EUSAGE.
- **Impact:** Major — the documented local command cannot build/check the console from a fresh checkout.
- **Fix:** Use Bun frozen installation and the existing check script in the target, update its help/docs, and verify the target from a clean dependency installation.
- **Effort:** S
- **Grade lift:** B− → B — local development entrypoint matches CI.
