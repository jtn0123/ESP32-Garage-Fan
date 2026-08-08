#pragma once
// The 2.13" e-ink FeatherWing: the at-a-glance panel on the device itself.
// A consumer like the web console -- it reads the other modules and renders,
// it owns nothing they need.

namespace display {

/** Initialise the panel (bracketed by a crash breadcrumb; epd init has hung). */
void begin();

/**
 * Refresh when due: every sample cycle, or 60 s after a speed change (e-ink
 * refreshes are slow and flashy; a per-change refresh would strobe). Reads
 * everything it shows straight from the owning modules.
 */
void maybe_render();

}  // namespace display
