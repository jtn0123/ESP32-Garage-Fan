// Bundle entry point. The build emits a classic IIFE, not an ES module: the
// page is served from the device over plain HTTP with no MIME nuance to spare,
// and a module script buys nothing when everything is inlined into one file.
import { boot } from './app.js';

boot().catch((err: unknown) => {
  // A failed boot leaves the static shell visible; say why in the console
  // rather than dying as an unhandled rejection.
  console.error('console boot failed:', err);
});
