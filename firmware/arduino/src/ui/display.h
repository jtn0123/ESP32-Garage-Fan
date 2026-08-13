#pragma once
// The 2.13" e-ink FeatherWing: the at-a-glance panel on the device itself.
// A consumer like the web console -- it reads the other modules and renders,
// it owns nothing they need.

#include <stddef.h>
#include <stdint.h>

namespace display {

/** Initialise the panel (bracketed by a crash breadcrumb; epd init has hung). */
void begin();

/**
 * Refresh when due: every sample cycle, or a settle period after a speed
 * change (e-ink refreshes are slow and flashy; a per-change refresh would
 * strobe). Reads everything it shows straight from the owning modules.
 */
void maybe_render();

/**
 * The last frame, exactly as it was handed to the panel.
 *
 * Two 1-bit planes, row-major, MSB first, a set bit meaning ink. This is OUR
 * format, not the driver's: Adafruit_EPD stores its buffer column-major and
 * rotated with per-driver inversion, so serving that would make the console's
 * mirror a re-derivation of a private implementation detail -- one that renders
 * something plausible and wrong the moment the driver changes. The panel and
 * the browser are fed from these same bytes instead.
 *
 * Null before the first render. `stride` is bytes per row.
 */
const uint8_t* plane_black();
const uint8_t* plane_red();
uint16_t width();
uint16_t height();
uint16_t stride();
size_t plane_bytes();

/** Seconds since the last refresh, or -1 if the panel has never rendered. */
int32_t age_s();

/**
 * Does this build drive a panel that can show red?
 *
 * Both the mono and tricolor 2.13" FeatherWings are SSD1680 at 250x122, so the
 * firmware is identical and only the glass differs. Everything red is an accent
 * over information already legible in black, so a mono panel loses decoration
 * and no meaning -- but the console still wants to know, so its mirror shows
 * what THIS device actually displays rather than an idealised version.
 */
bool tricolor();

/**
 * Force a refresh on the next loop pass, regardless of cadence.
 *
 * Returns false when refused: a refresh blocks the loop for seconds, so forced
 * repaints have a floor between them. Ask forced_retry_in_s() how long is left.
 */
bool request_refresh();

/** Seconds until request_refresh() would be accepted again; 0 when ready. */
uint32_t forced_retry_in_s();

}  // namespace display
