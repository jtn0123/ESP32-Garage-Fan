# UI Spec Authoring Guide

The display UI is defined once in `config/ui_spec.json` and rendered twice:
`scripts/gen_ui.py` generates the firmware op tables
(`firmware/arduino/src/ui_ops_generated.*`, rect table included) and the web
simulator bundle (`web/sim/ui_generated.js`). The simulator is the design
surface; the panel is the product. If they ever disagree, one of them has a
bug — that is the point of the pipeline.

## Workflow

1. Edit `config/ui_spec.json` (rects, components, variants).
2. `venv/bin/python scripts/gen_ui.py` — regenerates firmware + sim outputs.
   (Firmware builds also run it as a PlatformIO pre-script.)
3. Preview: start the sim (`.claude/launch.json` → `sim`, or
   `python3 -m http.server --directory web/sim`) and pick your variant from the
   **Variant** dropdown (or `?variant=<name>`).
4. Tests: `pytest tests/test_ui_validation.py tests/test_web_sim*.py`.
5. Deploy: `scripts/deploy.sh` — then verify on the glass via the remote
   screenshot channel (`espsensor/<id>/cmd/screenshot`).

Reference render: `docs/ui/v3_reference.png` is the owner-approved baseline.
Diff any visual change against it.

## Concepts

- **rects** — named `[x, y, w, h]` boxes on the 250×122 canvas. The firmware
  indexes them by a generated enum + `kRectTable`; never hand-maintain a
  rect lookup (that drifted once and blanked a whole variant on-device).
- **components** — named op lists. Group by purpose (`chrome_v3`,
  `header_v3`, `inside_v3`, ...), reuse across variants where identical
  (`footer_split` serves v2 and v3).
- **variants** — ordered component lists. `defaultVariant` is what the device
  boots into (page 0); the graphs page is the `v3g` variant. Variant order in
  the JSON maps to firmware variant ids — append new variants, never reorder.

## Op reference

| op | fields | notes |
|----|--------|-------|
| `line` | `from:[x,y] to:[x,y]` | horizontal/vertical only on device |
| `fill` | `rect`, `color?: "white"` | solid fill; basis of inverted bands |
| `text` | `rect, font, align, text, truncate?, color?: "inverse", when?` | `{placeholders}` resolved live |
| `textCenteredIn` | `rect, yOffset, font, text, color?, when?` | horizontally centered |
| `tempGroupCentered` | `rect, value` | big digits + °F; size-3 on rects ≥32 px tall |
| `iconIn` | `rect, iconFromWeather` | icon only — the label is always its own text op |
| `batteryGlyph` | `rect, percent` | 13×7 glyph, vertically centered |
| `timeRight` | `rect, source` | right-aligned time (legacy header) |
| `sparkline` | `rect, series, style?: "dashed"` | 24 h history polyline; series pairs sharing a rect share min/max |

- `when: "has(<field>)"` skips the op when the field has no live value.
- `color: "inverse"` draws white — use over a `fill`.
- Template fields resolve in `spec_format_field()` (firmware,
  `main.cpp`) and the sim's expander. **Adding a new `{field}` means adding it
  in both places** — a missing firmware key renders `--` on the glass while the
  sim looks fine (this happened; see fw 1.12→1.13 history).

## Sparkline series

`hist_temp_in`, `hist_temp_out`, `hist_rh_in`, `hist_rh_out` — fed by the
288-slot ring (`history_ring.h`), one slot per 5-minute sample, backfilled
from the daily CSVs at boot. NaN renders as a gap.

## Adding a variant, end to end

1. Add rects (suffix them, e.g. `_V4`; overlap with other variants' rects is
   fine — collisions only matter within one variant).
2. Add components; reuse existing ones where unchanged.
3. Append to `variants`; leave `defaultVariant` until the device should adopt.
4. Regenerate, preview via the dropdown, run the sim tests.
5. For device adoption: any *new* template fields need `spec_format_field()`
   support; flip `defaultVariant`; deploy; capture the glass.
