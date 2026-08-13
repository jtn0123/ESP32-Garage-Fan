import { defineConfig } from 'vitest/config';

/**
 * Vitest owns tests/ only.
 *
 * Its default glob is `**\/*.{test,spec}.ts`, which swallows the Playwright
 * specs in e2e/ -- they then fail on the first `page` fixture, in a runner that
 * has no browser. The two suites answer different questions (jsdom units vs the
 * shipped bundle in a real browser) and are run by different commands, so the
 * boundary is drawn here explicitly rather than left to a naming convention.
 */
export default defineConfig({
  test: {
    include: ['tests/**/*.test.ts'],
    exclude: ['node_modules/**', 'dist/**', 'e2e/**'],
    environment: 'jsdom',
  },
});
