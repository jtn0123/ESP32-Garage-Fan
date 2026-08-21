// The diagnostics strip along the bottom of the page, and the explanations
// behind it.
//
// Split from console.ts at the 500-line ceiling, along a seam the screen already
// has: this is the footer that answers "is the hardware healthy", while
// console.ts paints the live fan state above it. Every value here is derived
// from one DeviceState and written straight to the DOM.

import { $, el, show } from './dom.js';
import { cardTight, hoursMinutes, storage } from './format.js';
import { STATUS_BITS, view } from './state.js';
import { DIM, OK, OR, OUT, PU } from './theme.js';
import type { DeviceState } from './types.js';

/**
 * The SD bit's text: a size, or one of the two ways there is no card.
 *
 * "quarantined" is a distinct state worth its own word -- the driver disabled a
 * card whose mount crashed the board, which is not the same as no card being
 * fitted, and it is the one the user can act on.
 */
function cardBit(s: DeviceState): string {
  if (s.sd_total_mb) return storage(s.sd_used_mb, s.sd_total_mb, s.sd_free_mb);
  return s.sd_q ? 'quarantined' : 'not mounted';
}


/**
 * Link quality, in the three bands that mean something operationally.
 *
 * Around -61 dBm is solid; past -80 the MQTT session starts dropping and gaps
 * appear in the charts, which is the failure this colour is warning about.
 */
function rssiColour(dbm: number): string {
  if (dbm > -70) return OK;
  if (dbm > -80) return OR;
  return '#e0a9a9';
}

/** "⚡ 100% · 4.195 V" / "3.71 V" / "no pack" -- the VBAT bit. */
function battBit(batt: DeviceState['batt']): string {
  if (!batt) return 'no pack';
  const bolt = batt.chg ? '⚡ ' : '';
  const pct = batt.pct === null ? '' : `${batt.pct}% · `;
  return `${bolt}${pct}${batt.v.toFixed(3)} V`;
}

/** "20.3 W · 120.9 V" / "20.3 W" / "45.1 W · CYCLING ×5" / "no meter" -- the PLUG bit. */
export function plugBit(plug: DeviceState['plug']): string {
  if (!plug) return 'no meter';
  if (plug.cycling) return `${plug.w.toFixed(1)} W · CYCLING ×${plug.flips}`;
  const volts = plug.v === null ? '' : ` · ${plug.v.toFixed(1)} V`;
  return `${plug.w.toFixed(1)} W${volts}`;
}

/** Red only for a MEASURED fault (disagreement or cycling); absent hardware is not one. */
export function plugColour(plug: DeviceState['plug']): string {
  if (!plug) return DIM;
  return plug.verdict === -1 || plug.cycling ? '#e0a9a9' : OK;
}

/** The GAS status bit: off (dim), armed (quiet), or actively boosting (loud). */
function gasBit(s: DeviceState): { value: string; colour: string } {
  if (!s.gas_on) return { value: 'off', colour: DIM };
  if (s.gas_active) return { value: `BOOSTING ≥${s.gas_spd}`, colour: OR };
  return { value: `armed · trig ${s.gas_voc}`, colour: OUT };
}

function bitValues(): { value: string; colour: string }[] {
  const s = view.state;
  if (!s) return [];
  const card = cardBit(s);
  // A card with no room left stops being a flight recorder, so it reads as a
  // warning rather than as ordinary status text.
  const cardTense = s.sd_total_mb > 0 && cardTight(s.sd_used_mb, s.sd_total_mb);
  let cardColour = OUT;
  if (!s.sd_total_mb) cardColour = DIM;
  else if (cardTense) cardColour = OR;
  return [
    { value: `${s.rssi} dBm`, colour: rssiColour(s.rssi) },
    { value: card, colour: cardColour },
    { value: battBit(s.batt), colour: s.batt ? PU : DIM },
    {
      value: plugBit(s.plug),
      colour: plugColour(s.plug),
    },
    gasBit(s),
    { value: s.fw, colour: OUT },
    { value: hoursMinutes(s.uptime_s), colour: OUT },
    {
      value: `${s.slot} · ${s.confirmed ? 'confirmed' : 'unconfirmed'}`,
      colour: s.confirmed ? OK : OR,
    },
  ];
}

/**
 * Does this device actually hover?
 *
 * The status bits carried mouseenter/mouseleave AND click, which on a touch
 * screen is one gesture too many: a tap fires the pointer-enter compatibility
 * event first (opening the tip), then the click toggles it -- so the tip opened
 * and closed inside a single tap and all six dotted underlines were decoration
 * on a phone. Binding the hover pair only where hover exists leaves the desk
 * behaviour (read by hovering, which is the good one) untouched and lets the tap
 * be a plain toggle.
 */
const CAN_HOVER = window.matchMedia?.('(hover: hover)').matches ?? true;

export function paintBits(): void {
  const values = bitValues();
  if (!values.length) return;
  const host = $('stats');
  host.replaceChildren(
    ...STATUS_BITS.map((bit, n) => {
      const span = el('span', { className: 'bit' });
      span.append(el('b', { textContent: bit.key }));
      const v = el('span', { textContent: values[n]?.value ?? '' });
      v.style.color = values[n]?.colour ?? DIM;
      span.append(v);
      if (CAN_HOVER) {
        span.onmouseenter = () => {
          view.tip = n;
          paintTip();
        };
        span.onmouseleave = () => {
          view.tip = -1;
          paintTip();
        };
      }
      span.onclick = () => {
        view.tip = view.tip === n ? -1 : n;
        paintTip();
      };
      return span;
    }),
  );
  paintTip();
}

export function paintTip(): void {
  const bit = view.tip >= 0 ? STATUS_BITS[view.tip] : undefined;
  show($('tip'), !!bit);
  if (!bit) return;
  const title = $('tipt');
  title.textContent = bit.title;
  title.style.color = bitValues()[view.tip]?.colour ?? DIM;
  $('tipb').textContent = bit.body;
}
