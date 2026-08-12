import { defineConfig, devices } from '@playwright/test';

// A port of its own, so this can run beside tests/test_http_contract.py (8099)
// without the two fighting over one socket.
const PORT = Number(process.env['MOCK_PORT'] ?? 8100);

/**
 * A SECOND mock, used only by scenarios.spec.ts.
 *
 * Those tests flip global knobs -- `down=true` hangs up on every /api request
 * the process serves. The knobs live in module state shared by every connection,
 * so pointing them at the same server as everything else made unrelated specs
 * fail intermittently while a scenario happened to be mid-flight.
 * `describe.configure({ mode: 'serial' })` does not help: it orders tests inside
 * one file, it does not stop other FILES running at the same time. Separate
 * process, separate state.
 */
export const SCENARIO_PORT = PORT + 1;

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
  // Capped deliberately. Every worker drives a browser against ONE
  // single-process mock that rebuilds the whole history array per request, so
  // the default (half the cores, doubled across both projects) makes the mock
  // the bottleneck and the first assertion after load starts timing out. That
  // is a harness limit, not a bug in the console, and the honest fix is to stop
  // over-driving it rather than to paper over the timeouts with retries.
  workers: process.env['CI'] ? 2 : 4,
  reporter: process.env['CI'] ? [['github'], ['list']] : [['list']],
  timeout: 30_000,
  expect: { timeout: 10_000 },

  use: {
    baseURL: `http://127.0.0.1:${PORT}`,
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

  webServer: [
    {
      command: 'python3 ../scripts/mock_device.py',
      env: { MOCK_PORT: String(PORT) },
      url: `http://127.0.0.1:${PORT}/api/state`,
      reuseExistingServer: !process.env['CI'],
      stdout: 'pipe',
      stderr: 'pipe',
      timeout: 30_000,
    },
    {
      command: 'python3 ../scripts/mock_device.py',
      env: { MOCK_PORT: String(SCENARIO_PORT) },
      url: `http://127.0.0.1:${SCENARIO_PORT}/api/state`,
      reuseExistingServer: !process.env['CI'],
      stdout: 'pipe',
      stderr: 'pipe',
      timeout: 30_000,
    },
  ],
});
