import { defineConfig, devices } from '@playwright/test';

// A port of its own, so this can run beside tests/test_http_contract.py (8099)
// without the two fighting over one socket.
/**
 * No `webServer` here on purpose.
 *
 * mock_device.py keeps STATE and SCEN in module globals, and the console writes
 * to STATE constantly -- /api/set from the rail, /api/config from the drawer.
 * One shared server lets a write from one test land between another test's
 * action and its assertion, and `down=true` took the entire suite down with it.
 * e2e/harness.ts therefore starts a mock PER WORKER as a worker-scoped fixture
 * and sets baseURL from it: a worker runs one test at a time, so that is total
 * isolation and it needs no special cases.
 */

/**
 * End-to-end cover for the console, driven against scripts/mock_device.py.
 *
 * The mock serves web/dist/console.html verbatim -- the same bytes that get
 * embedded into src/generated_page.h and shipped to the board -- so these tests
 * exercise the artifact that actually deploys, not a dev server rendering of it.
 * `bun run test:e2e` rebuilds first so the bundle under test is current.
 *
 * What this can and cannot prove: the mock implements the SPEC, not the
 * firmware. Passing here means the console and the documented contract agree.
 * A divergence between the mock and the real device is exactly what an
 * on-hardware pass has to catch, which is why mock_device.py says so at the top.
 */
export default defineConfig({
  testDir: './e2e',
  fullyParallel: true,
  forbidOnly: !!process.env['CI'],
  retries: process.env['CI'] ? 2 : 0,
  // Each worker owns a mock process, so workers no longer contend for one
  // server -- but each is still a browser PLUS a Python process, which is why
  // this is not simply the core count.
  //
  // 3 on CI, not Playwright's default of 2: this suite is the slowest job in
  // the pipeline by a wide margin (~4m of a ~5m run), and it is almost all
  // waiting. Measured locally at 249 tests: 2 -> 212s, 3 -> 159s, 4 -> 131s.
  // A standard GitHub runner has 4 vCPUs, so 3 workers means 3 browsers and 3
  // mocks with a core still free; 4 would leave nothing for the runner itself
  // and trade wall-clock for flakiness, which is a bad trade in a suite whose
  // failures already cost a rerun.
  workers: process.env['CI'] ? 3 : 4,
  // CI also writes the HTML report: `github` annotates the failing line in the
  // diff and `list` scrolls past in the log, but neither survives as something
  // you can open afterwards. The workflow uploads playwright-report/ next to
  // the traces, and without this that path would be empty.
  reporter: process.env['CI']
    ? [['github'], ['list'], ['html', { open: 'never' }]]
    : [['list']],
  timeout: 30_000,
  expect: { timeout: 10_000 },

  use: {
    trace: 'on-first-retry',
    screenshot: 'only-on-failure',
  },

  projects: [
    {
      name: 'desktop',
      use: { ...devices['Desktop Chrome'] },
      // A desk browser has no touchscreen, so the gesture specs cannot run
      // here at all. Excluded rather than skipped at runtime: a skip that can
      // never become a pass is noise in every report, and noise in the skip
      // count is where a real skip would hide.
      testIgnore: /touch\.spec\.ts/,
    },
    // The console is used from a phone in the garage at least as often as from
    // a desk, and the layout has a real breakpoint. Running the whole suite in
    // both catches the "works until you shrink it" class of bug.
    {
      name: 'mobile',
      use: { ...devices['Pixel 7'] },
      // A phone cannot hover. Playwright will happily synthesise one, which is
      // how a tooltip that opens on hover and closes on the tap that follows it
      // stayed green here for two rounds while being dead on the device -- the
      // hover handlers are now bound only where `(hover: hover)` matches, so
      // these specs would be asserting a fiction. touch.spec.ts covers the same
      // affordances by tapping. (The desk project excludes touch.spec.ts for the
      // mirror-image reason.)
      testIgnore: /hover\.spec\.ts/,
    },
  ],

});
