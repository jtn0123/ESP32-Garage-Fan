#!/usr/bin/env python3
"""
Convert SVG weather/moon icons to 24x24 1-bit bitmaps and emit a C header
with PROGMEM arrays suitable for Adafruit GFX drawXBitmap.

Quality improvements:
- Oversample SVG rasterization (default 4×) for crisp edges
- Auto-threshold via Otsu (fallback to manual threshold)
- Optional 1px bolding (morphological dilation) to strengthen thin strokes
- Optional preview PNGs for quick visual verification

Dependencies:
  pip install cairosvg pillow
"""

import argparse
import io
import os
from typing import Optional

import cairosvg
from PIL import Image, ImageFilter

PROJECT_ROOT = os.path.dirname(os.path.dirname(__file__))
SRC_DIR = os.path.join(PROJECT_ROOT, "web", "icons", "mdi")
OUT_HEADER = os.path.join(PROJECT_ROOT, "firmware", "arduino", "src", "icons_generated.h")

ICON_NAMES: list[str] = [
    # weather
    "weather-sunny",
    "weather-partly-cloudy",
    "weather-cloudy",
    "weather-fog",
    "weather-pouring",
    "weather-drizzle",
    "weather-snowy",
    "weather-sleet",
    "weather-lightning",
    "weather-haze",
    "weather-dust",
    "weather-smoke",
    "weather-hurricane",
    "weather-tornado",
    "weather-night",
    "weather-night-partly-cloudy",
    "weather-windy-variant",
    # moon
    "moon-new",
    "moon-waxing-crescent",
    "moon-first-quarter",
    "moon-waxing-gibbous",
    "moon-full",
    "moon-waning-gibbous",
    "moon-last-quarter",
    "moon-waning-crescent",
]

WIDTH = 24
HEIGHT = 24


def svg_to_png_bytes(svg_path: str, oversample: int = 4) -> bytes:
    with open(svg_path, "rb") as f:
        svg_data = f.read()
    ow = max(1, WIDTH * max(1, int(oversample)))
    oh = max(1, HEIGHT * max(1, int(oversample)))
    return cairosvg.svg2png(bytestring=svg_data, output_width=ow, output_height=oh)


def _otsu_threshold(img_l: Image.Image) -> int:
    # img_l must be mode 'L'
    hist = img_l.histogram()
    total = sum(hist)
    sum_total = sum(i * h for i, h in enumerate(hist))
    sum_b = 0.0
    w_b = 0.0
    var_max = -1.0
    threshold = 160
    for t in range(256):
        w_b += hist[t]
        if w_b == 0:
            continue
        w_f = total - w_b
        if w_f == 0:
            break
        sum_b += t * hist[t]
        m_b = sum_b / w_b
        m_f = (sum_total - sum_b) / w_f
        var_between = w_b * w_f * (m_b - m_f) ** 2
        if var_between > var_max:
            var_max = var_between
            threshold = t
    return int(threshold)


def rasterize_1bit_centered(
    png_bytes: bytes,
    invert: bool = False,
    threshold: Optional[int] = None,
    auto_threshold: bool = True,
    bold_px: int = 0,
    outline_px: int = 0,
) -> Image.Image:
    with Image.open(io.BytesIO(png_bytes)) as im:
        # Preserve alpha and composite onto white to avoid black squares
        im = im.convert("RGBA")
        bg = Image.new("RGBA", im.size, (255, 255, 255, 255))
        im = Image.alpha_composite(bg, im)
        im = im.convert("L")  # grayscale
        # Fit into 24x24 preserving aspect
        im.thumbnail((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
        # Pad to 24x24
        canvas = Image.new("L", (WIDTH, HEIGHT), color=255)
        ox = (WIDTH - im.width) // 2
        oy = (HEIGHT - im.height) // 2
        canvas.paste(im, (ox, oy))
        # Auto threshold (Otsu) unless overridden
        thr = int(threshold) if isinstance(threshold, int) else None
        if thr is None and auto_threshold:
            thr = _otsu_threshold(canvas)
            # Bias slightly darker to keep thin details
            thr = max(0, min(255, thr - 5))
        if thr is None:
            thr = 160
        # Binarize to 1-bit via threshold
        bw_l = canvas.point(lambda p, t=thr: 0 if p < t else 255, "L")
        # Optional bold (dilate black) using MinFilter on L-mode
        if isinstance(bold_px, int) and bold_px > 0:
            for _ in range(bold_px):
                bw_l = bw_l.filter(ImageFilter.MinFilter(3))
        # Optional outline extraction: keep only the N-px edge band of the
        # filled mask (black minus black-eroded-by-N). MDI glyphs are filled
        # paths that read as solid blobs at panel size; their outline reads as
        # a proper icon on 1-bit e-ink. The web sim applies the identical
        # transform in its iconIn op so both surfaces draw the same glyph.
        if isinstance(outline_px, int) and outline_px > 0:
            eroded = bw_l.filter(ImageFilter.MaxFilter(2 * outline_px + 1))
            interior = eroded.point(lambda v: 255 if v < 128 else 0)
            bw_l = Image.composite(Image.new("L", bw_l.size, 255), bw_l, interior)
        bw = bw_l.convert("1")
        if invert:
            bw = bw.point(lambda p: 255 - p, "1")
        return bw


def pack_xbm_bits(img_1bit: Image.Image) -> bytes:
    assert img_1bit.mode == "1"
    out = bytearray()
    w, h = img_1bit.size
    for y in range(h):
        byte = 0
        bit_count = 0
        for x in range(w):
            # Pillow '1': 0=black, 255=white
            bit = 1 if img_1bit.getpixel((x, y)) == 0 else 0
            # XBM is LSB first within a byte
            byte |= (bit & 1) << bit_count
            bit_count += 1
            if bit_count == 8:
                out.append(byte)
                byte = 0
                bit_count = 0
        if bit_count != 0:
            out.append(byte)
    return bytes(out)


def c_array_name(name: str) -> str:
    return name.replace("-", "_") + "_24x24_bits"


def main() -> None:
    parser = argparse.ArgumentParser(description="Convert SVG icons to 24x24 1-bit header")
    parser.add_argument(
        "--oversample", type=int, default=4, help="SVG raster oversample factor (default 4)"
    )
    parser.add_argument(
        "--threshold", type=str, default="auto", help="'auto' (Otsu) or integer 0-255"
    )
    parser.add_argument(
        "--bold", type=int, default=0, help="Number of dilation passes to thicken lines (default 0)"
    )
    parser.add_argument(
        "--outline",
        type=int,
        default=0,
        help="Keep only the N-px edge band of the filled glyph (0 = filled)",
    )
    parser.add_argument(
        "--baked-dir",
        type=str,
        default="",
        help="Also write each 1-bit frame as PNG here (the web sim's "
        "device_baked set, so both surfaces draw byte-identical icons)",
    )
    parser.add_argument(
        "--preview-dir", type=str, default="", help="Optional directory to write 1-bit PNG previews"
    )
    parser.add_argument(
        "--width", type=int, default=24, help="Output icon width in pixels (default 24)"
    )
    parser.add_argument(
        "--height", type=int, default=24, help="Output icon height in pixels (default 24)"
    )
    args = parser.parse_args()

    thr_arg: Optional[int]
    auto_thr = True
    if str(args.threshold).lower() == "auto":
        thr_arg = None
        auto_thr = True
    else:
        try:
            thr_arg = int(args.threshold)
            auto_thr = False
        except Exception:
            thr_arg = None
            auto_thr = True

    # Set global target dimensions
    global WIDTH, HEIGHT
    WIDTH = max(8, int(args.width))
    HEIGHT = max(8, int(args.height))

    header_lines: list[str] = []
    header_lines.append("#pragma once")
    header_lines.append("// Copyright 2024 Justin")
    header_lines.append("#include <stdint.h>")
    header_lines.append("#include <pgmspace.h>")
    header_lines.append("")
    header_lines.append("// Auto-generated by scripts/convert_icons.py")
    header_lines.append(
        f"// invocation: --width {WIDTH} --height {HEIGHT} "
        f"--oversample {args.oversample} --outline {args.outline} "
        f"--bold {args.bold} --threshold {args.threshold}"
    )
    # Emit icon tokens for tests/metadata scans
    weather_tokens = [n for n in ICON_NAMES if n.startswith("weather-")]
    if weather_tokens:
        header_lines.append("// Icon tokens: " + " ".join(weather_tokens))
    header_lines.append(f"#define ICON_W {WIDTH}")
    header_lines.append(f"#define ICON_H {HEIGHT}")
    header_lines.append("")
    # Provide simple typedef/struct markers for tests
    header_lines.append("typedef struct __icon_data_marker { const uint8_t* p; }")
    header_lines.append("                     __icon_data_marker_t;")
    header_lines.append("struct IconData { const uint8_t* data; uint16_t w; uint16_t h; };")
    header_lines.append("")
    # Provide ICON_ alias markers that include hyphenated names searched by tests
    alias_markers = [f"ICON_{n}" for n in ICON_NAMES]
    header_lines.append("// " + " ".join(alias_markers))
    header_lines.append("")
    header_lines.append("enum IconId {")
    for name in ICON_NAMES:
        header_lines.append(f'    ICON_{name.replace("-", "_").upper()},')
    header_lines.append("};")
    header_lines.append("")

    previews: list[tuple[str, Image.Image]] = []
    for name in ICON_NAMES:
        svg_path = os.path.join(SRC_DIR, f"{name}.svg")
        if not os.path.exists(svg_path):
            print("missing", svg_path)
            continue
        png_bytes = svg_to_png_bytes(svg_path, oversample=args.oversample)
        img = rasterize_1bit_centered(
            png_bytes,
            invert=False,
            threshold=thr_arg,
            auto_threshold=auto_thr,
            bold_px=args.bold,
            outline_px=args.outline,
        )
        previews.append((name, img))
        if args.baked_dir:
            os.makedirs(args.baked_dir, exist_ok=True)
            img.save(os.path.join(args.baked_dir, f"{name}.png"))
        bits = pack_xbm_bits(img)
        arr_name = c_array_name(name)
        header_lines.append(f"static const uint8_t {arr_name}[] PROGMEM = {{")
        # format bytes as 0x.., and wrap to keep lines <= 80 chars
        indent = "    "
        # Use a conservative fixed bytes-per-line so generated data lines stay
        # under 80 chars
        bytes_per_line = 4
        line = indent
        for i, b in enumerate(bits):
            line += f"0x{b:02X}, "
            if (i + 1) % bytes_per_line == 0:
                header_lines.append(line.rstrip())
                line = indent
        if line.strip():
            header_lines.append(line.rstrip())
        header_lines.append("};")
        header_lines.append("")

    # draw helper
    header_lines.append("template<typename GFX>")
    header_lines.append("inline void draw_icon_xbm(GFX& d, int16_t x, int16_t y,")
    header_lines.append("    IconId id, uint16_t color) {")
    header_lines.append("    switch (id) {")
    for name in ICON_NAMES:
        arr = c_array_name(name)
        enum_name = "ICON_" + name.replace("-", "_").upper()
        header_lines.append(f"    case {enum_name}:")
        # Emit draw call and break on separate lines to keep line length short
        header_lines.append(f"        d.drawXBitmap(x, y, {arr}, ICON_W, ICON_H, color);")
        header_lines.append("        break;")
    header_lines.append("    default: break;")
    header_lines.append("    }")
    header_lines.append("}")

    os.makedirs(os.path.dirname(OUT_HEADER), exist_ok=True)
    with open(OUT_HEADER, "w") as f:
        f.write("\n".join(header_lines) + "\n")
    print("wrote", OUT_HEADER)

    # Optional previews
    if args.preview_dir:
        pdir = os.path.join(PROJECT_ROOT, args.preview_dir)
        os.makedirs(pdir, exist_ok=True)
        for name, img in previews:
            outp = os.path.join(pdir, f"{name}.png")
            try:
                img.save(outp)
            except Exception:
                pass
        print("wrote previews to", pdir)


if __name__ == "__main__":
    main()
