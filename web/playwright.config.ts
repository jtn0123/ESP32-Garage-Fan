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
 * `npm run test:e2e` rebuilds first so the bundle under test is current.
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
  // Each worker now owns a mock process, so workers no longer contend for one
  // server -- but each one is still a browser plus a Python process, so this
  // stays modest rather than defaulting to half the cores.
  workers: process.env['CI'] ? 2 : 4,
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
    { name: 'desktop', use: { ...devices['Desktop Chrome'] } },
    // The console is used from a phone in the garage at least as often as from
    // a desk, and the layout has a real breakpoint. Running the whole suite in
    // both catches the "works until you shrink it" class of bug.
    { name: 'mobile', use: { ...devices['Pixel 7'] } },
  ],

});
