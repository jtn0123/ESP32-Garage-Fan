// The e-ink mirror: what the panel on the wall is showing, in the browser.
//
// The device sends the same two 1-bit planes it clocked to the glass, so this
// cannot drift from the real display. Everything here is decode-and-blit; there
// is deliberately no layout logic, because a re-implementation of the layout is
// exactly the thing that goes stale without anyone noticing.

import type { DisplayFrame } from './types';

/** Colours chosen to read as e-ink paper rather than as a screen. */
const PAPER = '#e8e4dc';
const INK = '#1b1b1a';
const RED = '#c4302b';

/** Decode base64 into bytes. atob is fine here: the payload is ours and small. */
export function decodePlane(b64: string): Uint8Array {
  const bin = atob(b64);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

/** Is the bit at (x, y) set in a row-major, MSB-first 1bpp plane? */
export function bitAt(plane: Uint8Array, stride: number, x: number, y: number): boolean {
  const byte = plane[y * stride + (x >> 3)];
  if (byte === undefined) return false;
  return (byte & (0x80 >> (x & 7))) !== 0;
}

/**
 * Paint a frame into a canvas at `scale`.
 *
 * Nearest-neighbour on purpose: an e-ink panel has hard pixels and smoothing
 * them would make the mirror prettier than the thing it mirrors. Red is drawn
 * over black, matching the panel, where a pixel set in both planes shows red.
 */
export function paintFrame(canvas: HTMLCanvasElement, frame: DisplayFrame, scale = 2): void {
  if (!frame.ready || frame.w === 0 || frame.h === 0) return;
  const black = decodePlane(frame.black);
  const red = decodePlane(frame.red);
  const { w, h, stride } = frame;

  canvas.width = w * scale;
  canvas.height = h * scale;
  const ctx = canvas.getContext('2d');
  if (!ctx) return;

  ctx.fillStyle = PAPER;
  ctx.fillRect(0, 0, canvas.width, canvas.height);

  const img = ctx.createImageData(w, h);
  const [pr, pg, pb] = hexToRgb(PAPER);
  const [ir, ig, ib] = hexToRgb(INK);
  const [ar, ag, ab] = hexToRgb(RED);

  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 4;
      let r = pr;
      let g = pg;
      let b = pb;
      if (bitAt(black, stride, x, y)) {
        r = ir;
        g = ig;
        b = ib;
      }
      // Red last, and ONLY on glass that has red ink. On the mono part the
      // colour plane is never clocked out, so those pixels are blank paper --
      // painting them a stand-in grey here would show an accent the wall does
      // not have, and a mirror that invents pixels is worse than no mirror.
      // This is why the speed bar carries a black hatch under its red fill:
      // on mono the hatch is what remains, and the bar still reads.
      if (frame.tricolor && bitAt(red, stride, x, y)) {
        r = ar;
        g = ag;
        b = ab;
      }
      img.data[i] = r;
      img.data[i + 1] = g;
      img.data[i + 2] = b;
      img.data[i + 3] = 255;
    }
  }

  // Blit at 1:1 into an offscreen canvas, then scale up with smoothing off.
  const off = document.createElement('canvas');
  off.width = w;
  off.height = h;
  off.getContext('2d')?.putImageData(img, 0, 0);
  ctx.imageSmoothingEnabled = false;
  ctx.drawImage(off, 0, 0, canvas.width, canvas.height);
}

function hexToRgb(hex: string): [number, number, number] {
  const n = parseInt(hex.slice(1), 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}

/**
 * How stale the mirror is, in words.
 *
 * The panel only refreshes on the sample cadence, so "3 minutes ago" is normal
 * and not a fault — saying so stops the mirror looking broken when it is merely
 * waiting, which is most of the time.
 */
export function ageText(frame: DisplayFrame): string {
  if (!frame.ready || frame.age_s < 0) return 'waiting for the first refresh';
  if (frame.age_s < 60) return `refreshed ${frame.age_s}s ago`;
  const m = Math.floor(frame.age_s / 60);
  return `refreshed ${m}m ago`;
}
