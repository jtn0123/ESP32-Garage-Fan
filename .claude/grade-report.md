# Codebase Grade Report

**Project:** ESP32-Garage-Fan
**Audited:** 2026-08-09
**Stack:** C++ (Arduino/PlatformIO on ESP32-S2) firmware + strict-TypeScript web console (zero runtime deps, esbuild) + Python build/test scripts

All grades are backed by checks run today on a synced tree: both firmware envs build
warning-free, 91 tests green (31 pytest + 47 vitest + 13 native Unity), all five
linters clean, committed bundle current.

## Summary

| ID | Category | Grade | Items |
|----|----------|-------|-------|
| A | Architecture & Design | A− | 2 |
| B | Backend Quality (firmware services/API) | B+ | 1 |
| C | Frontend Quality | A− | 2 |
| D | Testing & Reliability | B | 4 |
| E | Security | B− | 4 |
| F | Dependencies & Tech Currency | B+ | 3 |
| G | Performance & Scalability | A− | 2 |
| H | Documentation & Onboarding | A− | 2 |
| I | Developer Experience & Tooling | A− | 2 |
| **Overall** | | **B+** | **22** |

**Top 5 highest-leverage fixes:** E1, E2, D2, D1, C1

**Execution status (2026-08-09):** ~~D1~~ ~~D2~~ ~~C1~~ ✓ done (branch
`claude/stability-logging`, +44 tests). E1/E2 declined by owner — LAN-only
network, no auth wanted at this point. Bonus same-day: flight-recorder event
log, WiFi self-heal ladder, SD format frequency-ladder fix (a live 2026-08-09
failure), and the A1 web.cpp split.

Overall is B+ not A− because Security (B−) and Testing (B) weigh more than the
strong code-quality categories: the mutation endpoints are unauthenticated and the
riskiest embedded logic (battery windowing — which just had a real bug — and the
history ring) has no tests.

---

## A — Architecture & Design — A−

One concern per module with statics behind narrow namespace APIs
(`firmware/arduino/src/{fan,sensors,storage,net,ui,system}/`), a 162-line
orchestrator main, and dependency inversion where it matters (`fan::set_notify`,
`sse::set_state_source` keep fan logic network-free). The build-artifact chain
(TS → committed bundle → gzipped header) is one-directional and CI-enforced.
Every file honors the 500-line ceiling — but one is at 96% of it.

#### A1 — Split net/web.cpp before it breaks the 500-line ceiling
- **Where:** `firmware/arduino/src/net/web.cpp` (480 lines today)
- **What's wrong:** It holds route registration, all handlers, `state_json`, and the OTA upload state machine. Any new endpoint pushes it over the project's hard 500-line rule.
- **Fix:** Extract the OTA upload handlers (`handle_update_*`, ~line 353–430) into `net/web_ota.cpp` behind `web_ota::register_routes(WebServer&, const char* token)` — same pattern as `web_debug`. Move-only; verify with flash-delta ≈ 0 and the contract test.
- **Effort:** S
- **Grade lift:** A− → A (removes the only ceiling risk in the tree)

#### A2 — config.h mixes five modules' constants
- **Where:** `firmware/arduino/src/config.h` (pins, duty table, MQTT topics, cadences, ring size in one header)
- **What's wrong:** Every module recompiles when any constant changes, and ownership is muddied — the duty table is fan/'s ground truth but lives in the shared grab-bag.
- **Fix:** Keep `config.h` for cross-cutting identity (hostname, version, repo) and move per-module constants next to their owners (`fan/protocol_table.h`, `net/topics.h`). Purely mechanical; contract test and duty-table pin test guard it.
- **Effort:** S
- **Grade lift:** A− → A− (hygiene; no functional change)

---

## B — Backend Quality (firmware services/API) — B+

Handlers are thin, sends are bounded with truncation aborts (`net/http_tx`), SD
reads feed the watchdog, NVS writes happen only on change, and OTA is A/B with
broker-confirmed rollback. The API-design wart (side-effectful GETs) is graded
under E1 since the fix is the same change. The one structural gap:

#### B1 — WiFi credentials are compile-time only; no field recovery path
- **Where:** `scripts/gen_device_header.py` → `generated_config.h`; consumed in `firmware/arduino/src/net/`
- **What's wrong:** Changing SSID/password requires a laptop, the repo, `.env`, and a USB cable in the garage. A router change strands the device (this already bit once: an empty-credential image bricked networking until USB reflash — see the guard comment in `scripts/deploy.sh:57`).
- **Fix:** On repeated WiFi join failure, fall back to a captive-portal AP that stores credentials in NVS; baked credentials become the first-boot default. Sizeable: touches boot order in main and needs flash-headroom review (see G1) first.
- **Effort:** L
- **Grade lift:** B+ → A− (removes the only unrecoverable-in-field failure mode)

---

## C — Frontend Quality — A−

Strict TypeScript (`noUncheckedIndexedAccess`, `exactOptionalPropertyTypes`), zero
runtime dependencies, every module under 400 lines, declarative settings rows with
aria labels and `role=switch`, injected `Storage` for testability, `AbortSignal`
timeouts on fetches. Two real items:

#### C1 — Chart x-axis spaces samples by index, not time
- **Where:** `web/src/series.ts` (`mergeCache`), consumed by `web/src/charts.ts`
- **What's wrong:** A missed 5-minute sample (reboot, WiFi drop) compresses the time axis: points sit at even spacing regardless of the real gap, so slopes lie around outages. Flagged during review and deferred.
- **Fix:** Space points by timestamp; render `null` gaps as breaks in the line rather than connecting across them. Extend the existing `series.test.ts` fixtures with a gapped day.
- **Effort:** M
- **Grade lift:** A− → A (chart honesty around exactly the events you'd investigate)

#### C2 — Installable PWA shell without a service worker
- **Where:** `firmware/arduino/src/net/web.cpp` (`kManifest` at file scope); no `sw.js` anywhere in `web/src/`
- **What's wrong:** The manifest makes the console installable, but with no service worker an installed app shows a browser error page whenever the device is down — exactly when you want a "device offline" screen.
- **Fix:** Minimal service worker: cache the shell on install, serve it on fetch failure with an offline banner; add to the esbuild bundle. Low priority — the page is served by the device itself, so offline utility is limited to a nicer failure screen.
- **Effort:** M
- **Grade lift:** A− → A− (polish)

---

## D — Testing & Reliability — B

Strong where it exists: the wire-contract pytest pins every firmware JSON writer to
`web/src/types.ts`, native Unity covers the auto-thermostat, OTA rollback, and
boot-health decisions, vitest covers format/series/version/update logic, and CI
gates warnings, bundle drift, and lint at zero. The gap is one-sided: the riskiest
*embedded* logic is untested.

#### D1 — [BE] History ring and temp_stats have no tests
- **Where:** `firmware/arduino/src/storage/history.cpp` (ring of 288 `Sample`s, wrap logic, min/max/avg)
- **What's wrong:** Wrap-around, partial-fill, and stats edge cases (all-NaN day, single sample) are exactly the bugs that corrupt charts silently, and nothing exercises them.
- **Fix:** Extract the ring+stats into a header testable without Arduino.h (pattern: `fan/auto_logic.h`), add a `native_history` env to `platformio.ini`; `scripts/test-native.sh` auto-discovers it.
- **Effort:** M
- **Grade lift:** B → B+

#### D2 — [BE] Battery windowing/charging verdict is untested and already had a bug
- **Where:** `firmware/arduino/src/sensors/battery.cpp` (percent history window, NAN seeding, sticky charging verdict, ETA)
- **What's wrong:** Review round two found a real defect here (unknown percent stored as 0, skewing the charge verdict). It was fixed by hand-reasoning — nothing stops it regressing.
- **Fix:** Same extraction pattern as D1: pure windowing/verdict logic into a header + `native_battery` env; first cases = the NAN-seed regression and a charge/discharge flip.
- **Effort:** M
- **Grade lift:** B → B+ (tests where a bug demonstrably lived)

#### D3 — [FE] Painters and app wiring have zero coverage
- **Where:** `web/src/console.ts` (376 lines), `web/src/app.ts` (344 lines) — vitest covers the pure logic modules only
- **What's wrong:** A typo in DOM ids or a painter crash ships silently; tsc catches types, not wiring.
- **Fix:** One jsdom smoke test: mount the real HTML shell, boot with a mocked fetch returning a canned `/api/state`, assert key elements render and no exception escapes `boot()`.
- **Effort:** S
- **Grade lift:** B → B+ (cheap crash-detector for the biggest untested files)

#### D4 — [BE] Climate self-heating offset selection untested
- **Where:** `firmware/arduino/src/sensors/climate.cpp` (offset keyed by `battery::charging()`, outdoor-epoch pairing window)
- **What's wrong:** The charging/not-charging offset switch and the 10 s epoch-pairing rule are subtle, load-bearing for displayed temperature, and unexercised.
- **Fix:** Fold into the D1/D2 extraction round; table-driven cases for both offsets and a stale-epoch rejection.
- **Effort:** S (once D1/D2 establish the pattern)
- **Grade lift:** B+ → A− (with D1–D3 done)

---

## E — Security — B−

Real strengths: empty-token guard (`web.cpp:67`), destructive routes are POST-only
(`/api/restart`, `/api/sdformat`, `/update`), debug routes token-gated, gitleaks
with tuned rules, SHA-pinned actions, and release binaries deliberately carry no
credentials. But the everyday mutation surface is open:

#### E1 — Fan mutations are unauthenticated GETs → LAN drive-by
- **Where:** `firmware/arduino/src/net/web.cpp:451,453` — `/api/set` and `/api/config` registered with no method filter and no `token_ok()`
- **What's wrong:** Any web page you visit can flip the fan or rewrite thresholds via `<img src="http://garage-fan.local/api/set?speed=12">` (classic CSRF/DNS-rebind against LAN devices). This was an explicit review-time decision (convenience curl-ability) — worth revisiting now that the console uses `fetch` anyway.
- **Fix:** Accept POST on both (console's `api.ts` already has `guarded()` POST plumbing), keep GET read-only or gate GET writes behind the token; update `tests/test_web_contract.py` and the README API bullet.
- **Effort:** M
- **Grade lift:** B− → B+ (closes the only unauthenticated write path)

#### E2 — Public default OTA token still bootable
- **Where:** `firmware/arduino/src/config.h` (`#ifndef FAN_OTA_TOKEN "iliving-ota"`), warned about in `scripts/deploy.sh:31`
- **What's wrong:** A device built without `.env` accepts firmware from anyone on the LAN who read this public repo. deploy.sh warns, but a warning is not a gate.
- **Fix:** In firmware: if the compiled token equals the public default, serve 403 on `/update` with a "set FAN_OTA_TOKEN and reflash over USB" body. CI builds stay compilable; the default becomes read-only-safe.
- **Effort:** S
- **Grade lift:** with E1: B+ → A− (both LAN write paths gated)

#### E3 — Token travels in the query string
- **Where:** `web.cpp:206,353,399` (`g_http.arg("token")`), `scripts/deploy.sh:74`
- **What's wrong:** `?token=` lands in browser history, proxy logs, and referer-adjacent surfaces. LAN-only and plain HTTP anyway, so marginal — but free to fix while doing E1.
- **Fix:** Read the token from an `X-OTA-Token` header first, query param as fallback; update deploy.sh and `api.ts`.
- **Effort:** S
- **Grade lift:** hygiene; no letter change alone

#### E4 — No CVE watch on the npm toolchain
- **Where:** `.github/dependabot.yml` — ecosystems `pip` and `github-actions` only; `web/` unwatched
- **What's wrong:** esbuild/vitest/typescript are dev-only, but they *are* the build chain that produces bytes flashed to the device. Advisories arrive silently today.
- **Fix:** Add an `npm` ecosystem entry for `/web` to dependabot.yml.
- **Effort:** S
- **Grade lift:** B− → B− (small, but closes the monitoring gap)

---

## F — Dependencies & Tech Currency — B+

Discipline is excellent: every GitHub Action SHA-pinned with version comments, the
espressif platform pinned to a release artifact, `package-lock.json` committed, CI
tool installs pinned, zero runtime npm deps. Currency is the lag: typescript 5.9 vs
7.0, vitest 3 vs 4, jsdom 26 vs 29 (majors, dev-only — from `npm outdated` today).

#### F1 — Dependabot doesn't watch web/
- **Where:** `.github/dependabot.yml`
- **What's wrong:** Same fact as E4, filed once there for the security angle; the currency consequence is that dev-dep majors drift silently (three are behind right now).
- **Fix:** Executing E4 resolves this item too.
- **Effort:** S
- **Grade lift:** B+ → A− (pins currency to a bot instead of memory)

#### F2 — Dev toolchain one major behind across the board
- **Where:** `web/package.json` — typescript 5.9.3→7.0.2, vitest 3.2.7→4.1.10, jsdom 26→29
- **What's wrong:** All dev-only, nothing broken, but major-version drift compounds: the jump gets riskier the longer it waits.
- **Fix:** One PR: bump all four, `npm run typecheck && npm test && npm run build`, commit the (likely byte-identical) bundle. TypeScript 7 is the one to read release notes for.
- **Effort:** S
- **Grade lift:** B+ → A−

#### F3 — Platform pin has no update signal
- **Where:** `firmware/arduino/platformio.ini` — platform = pioarduino release-zip URL (55.03.311)
- **What's wrong:** URL pins are immune to dependabot, so espressif core fixes (including security fixes in the WiFi stack) arrive only if someone remembers to look.
- **Fix:** Note the bump cadence in CLAUDE.md (e.g. check quarterly), or add a scheduled CI job that compares the pinned version against the pioarduino latest release and opens an issue.
- **Effort:** S
- **Grade lift:** hygiene

---

## G — Performance & Scalability — A−

Right-sized everywhere: the page gzips 52→18 KB and is served from flash with
`Content-Encoding: gzip`; the RMT peripheral keeps the fan waveform alive with zero
CPU (even during flash writes); RAM sits at 27%; sends are bounded. The single
pressure point is flash at 79.4% of the app partition.

#### G1 — Flash headroom audit before the next feature
- **Where:** build output (1,144,888 / 1,441,792 bytes); `firmware/arduino/platformio.ini`
- **What's wrong:** 20% headroom is fine today but B1 (captive portal) or any TLS-adjacent library would eat it. Nobody has itemized where the 1.1 MB goes.
- **Fix:** Run PlatformIO's inspect/size on the controller env, list the top object files, and check for linkable-but-unused Arduino components (e.g. classic BT tables shouldn't exist on S2, but Wire/SPI/USB stacks may drag extras). Document findings in HARDWARE.md's firmware section.
- **Effort:** M
- **Grade lift:** A− → A− (information, not speed — it protects future features)

#### G2 — 30-day history pulls block the single HTTP loop
- **Where:** `firmware/arduino/src/storage/sdcard.cpp` (`read_range`), served via `net/http_tx::send_big`
- **What's wrong:** WebServer is single-threaded: while a 30-day SD read streams, `/api/state`, SSE, and MQTT servicing in `loop()` all wait. The watchdog is fed, so nothing crashes — but the console freezes during its own chart load. Measure before optimizing.
- **Fix:** Time a 30-day pull on hardware. If it's >1–2 s, either cap `days` per request and page from the client, or yield to `loop()` between chunks.
- **Effort:** S (measure) then M (if warranted)
- **Grade lift:** A− → A (removes the one interactive stall)

---

## H — Documentation & Onboarding — A−

README is current, fan-specific, and honest (protocol warning, power gotchas, build
table). `docs/HARDWARE.md` (added yesterday, 186 lines) covers wiring, the polarity
gotcha, power laws, and a rebuild checklist. `docs/fan_protocol/PROTOCOL.md` plus
raw captures is reference-grade reverse-engineering documentation. CLAUDE.md keeps
agents honest. Two gaps:

#### H1 — HTTP API documented only as a README bullet
- **Where:** `README.md:24-26`; the real shapes live in `web/src/types.ts` and `web.cpp`
- **What's wrong:** Endpoint list exists but no request/response examples — anyone scripting against the device (or debugging with curl) must read source. Note the gzip caveat: bare `curl /` needs `--compressed`.
- **Fix:** `docs/API.md`: one section per endpoint with a real JSON response (copy from the contract-test fixtures), token rules, and the curl `--compressed` note. Link from README.
- **Effort:** S
- **Grade lift:** A− → A

#### H2 — Web toolchain expectations undocumented
- **Where:** README "Build" section covers `make web` but not node version or the CI drift-check contract
- **What's wrong:** A contributor who edits TS and forgets to commit the rebuilt bundle learns about the drift check only when CI fails.
- **Fix:** Three lines in README: node ≥ 22, `npm --prefix web ci`, and "commit `web/dist/console.html` — CI verifies it matches source."
- **Effort:** S
- **Grade lift:** polish

---

## I — Developer Experience & Tooling — A−

The loop is genuinely good: `make` targets for build/flash/deploy/test/web/lint,
`scripts/test-native.sh` auto-discovers native envs, strict TS gives instant
feedback, deploy.sh verifies the device actually came back confirmed and explains
each failure mode in plain language. CI is thorough (warnings gate, drift check,
lint matrix, contract test) and fast to reason about.

#### I1 — No pre-commit hooks; formatting failures surface in CI
- **Where:** repo root — no `.pre-commit-config.yaml`
- **What's wrong:** clang-format/black/ruff drift is caught minutes later by CI instead of seconds later locally; each miss costs a push-fix-push cycle.
- **Fix:** `.pre-commit-config.yaml` with ruff, black, clang-format, and gitleaks; document `pre-commit install` in README. Keep CI as the authority.
- **Effort:** S
- **Grade lift:** A− → A

#### I2 — Dead SonarCloud check muddies PR status
- **Where:** SonarCloud's server-side automatic analysis (phantom project `jtn0123_ESP32-Garage-Fan2`) + empty `SONAR_TOKEN` secret; the in-repo step already self-skips (`ci.yml:229`)
- **What's wrong:** Every PR shows a failing third-party check that nothing in the repo can fix — trains people to ignore red.
- **Fix:** In the SonarCloud UI: delete the phantom project and either configure the real one with a token or disconnect the integration. (Task chip already filed.)
- **Effort:** S (external UI access required)
- **Grade lift:** A− → A (green means green again)
