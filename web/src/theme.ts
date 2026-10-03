// Colours the canvas needs. The CSS carries the same values as custom
// properties; canvas cannot read those without a getComputedStyle call per
// frame, so they are duplicated here deliberately. Keep the two in step --
// console.css is the other half.
export const AC = '#3b82f6'; // accent / fan
export const OK = '#22a06b'; // healthy / pressure
export const OR = '#e8834a'; // garage temperature
export const PU = '#b98add'; // battery
export const OUT = '#8fa3b8'; // outside temperature
export const RH = '#6ea8fe'; // humidity
export const DIM = '#7d8795'; // axis labels (5.32:1 on --bg; see console.css)
export const TX = '#e6e9ed'; // foreground
export const FAI = '#757f8c'; // inactive control labels (4.77:1)

/**
 * One colour per chart row, and the underline on that row's chip. No row may
 * borrow another entity's colour: power wore the garage orange, VOC the
 * pressure green and NOx the battery violet, so a strip with every chip on
 * said three things twice. The three replacements clear colour-blind
 * separation (CVD ΔE >= 8) against their strip neighbours, and the power
 * yellow clears plain separation from the garage orange (ΔE 16.6).
 */
export const SERIES_COLOURS = {
  fan: AC,
  humidity: RH,
  pressure: OK,
  battery: PU,
  power: '#d8c228',
  voc: '#d55181',
  nox: '#1fa6b8',
} as const;

/** Chart gutters: left leaves room for a 4-digit pressure label at 10px mono. */
export const PAD_LEFT = 46;
export const PAD_RIGHT = 8;
