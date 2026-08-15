"""The mock's e-ink mirror: a synthetic frame in the firmware's plane
format, including the Python twin of the firmware's fixed V1-L QR encoder
(ui/qr_v1.h). THE TWO ENCODERS ARE A MATCHED PAIR pinned to segno by
tests/test_qr_v1.py and test_qr_v1.cpp -- never refactor one side alone.
Split from mock_device.py at the 500-line ceiling."""

from __future__ import annotations

from mock_state import STATE

# ------------------------------------------------------- the e-ink mirror
#
# A SYNTHETIC frame, not a reimplementation of ui/display.cpp. The console's
# mirror is a framebuffer blitter with no layout logic of its own (see
# web/src/panel.ts), so what it needs exercising is the wire shape and the
# decode: two 1-bit planes, row-major, MSB first, correct stride, red over
# black. Copying the firmware's layout here would be a second copy to keep in
# step, which is the exact trap the framebuffer design avoids.
#
# It does track STATE["speed"], so the bar and the digits move when the console
# drives the fan -- otherwise a test could not tell a live mirror from a
# hardcoded picture.
DISP_W = 250
DISP_H = 122
DISP_STRIDE = (DISP_W + 7) // 8

# 3x5 digits, one string per glyph, row-major. Enough to read a speed off the
# mock's panel; the real panel uses the Adafruit GFX font.
_FONT3X5 = {
    # Letters for the banner knockout ("GARAGE FAN").
    "G": ("111", "100", "101", "101", "111"),
    "A": ("010", "101", "111", "101", "101"),
    "R": ("110", "101", "110", "101", "101"),
    "E": ("111", "100", "110", "100", "111"),
    "F": ("111", "100", "110", "100", "100"),
    "N": ("101", "111", "111", "101", "101"),
    " ": ("000", "000", "000", "000", "000"),
    "0": ("111", "101", "101", "101", "111"),
    "1": ("010", "110", "010", "010", "111"),
    "2": ("111", "001", "111", "100", "111"),
    "3": ("111", "001", "111", "001", "111"),
    "4": ("101", "101", "111", "001", "001"),
    "5": ("111", "100", "111", "001", "111"),
    "6": ("111", "100", "111", "101", "111"),
    "7": ("111", "001", "001", "001", "001"),
    "8": ("111", "101", "111", "101", "111"),
    "9": ("111", "101", "111", "001", "111"),
    "-": ("000", "000", "111", "000", "000"),
    ".": ("000", "000", "000", "000", "100"),
}


class _Plane:
    """A 1-bit, row-major, MSB-first plane -- the firmware's format."""

    def __init__(self) -> None:
        self.buf = bytearray(DISP_STRIDE * DISP_H)

    def px(self, x: int, y: int, on: bool = True) -> None:
        if 0 <= x < DISP_W and 0 <= y < DISP_H:
            if on:
                self.buf[y * DISP_STRIDE + (x >> 3)] |= 0x80 >> (x & 7)
            else:
                self.buf[y * DISP_STRIDE + (x >> 3)] &= ~(0x80 >> (x & 7)) & 0xFF

    def rect(self, x: int, y: int, w: int, h: int, on: bool = True) -> None:
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.px(xx, yy, on)

    def frame(self, x: int, y: int, w: int, h: int) -> None:
        for xx in range(x, x + w):
            self.px(xx, y)
            self.px(xx, y + h - 1)
        for yy in range(y, y + h):
            self.px(x, yy)
            self.px(x + w - 1, yy)

    def text(self, x: int, y: int, s: str, scale: int = 1, on: bool = True) -> None:
        for ch in s:
            glyph = _FONT3X5.get(ch)
            if glyph:
                for gy, row in enumerate(glyph):
                    for gx, bit in enumerate(row):
                        if bit == "1":
                            self.rect(x + gx * scale, y + gy * scale, scale, scale, on)
            x += 4 * scale


# ---- V1-L alphanumeric QR, mask 0: the same fixed encoder the firmware
# carries in ui/qr_v1.h, ported line for line. tests/test_qr_v1.py pins this
# against a matrix from the `segno` reference library; if you change one port,
# change both and re-pin.
_QR_ALNUM = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:"
QR_SIZE = 21
_QR_DATA_CW, _QR_ECC_CW = 19, 7


def _gf_mul(a: int, b: int) -> int:
    r = 0
    while b:
        if b & 1:
            r ^= a
        a <<= 1
        if a & 0x100:
            a ^= 0x11D
        b >>= 1
    return r


# The complexity IS the QR spec, and this is a line-for-line twin of
# ui/qr_v1.h -- the pair must never be refactored one side alone
# (tests/test_qr_v1.py pins both to segno's matrices).
def qr_v1_encode(text: str) -> list[list[int]] | None:  # NOSONAR -- see twin-pair note above
    """21x21 matrix of 0/1 for `text`, or None if it does not fit V1-L."""
    if not text or len(text) > 25 or any(c not in _QR_ALNUM for c in text):
        return None
    bits: list[int] = []

    def put(val: int, n: int) -> None:
        bits.extend((val >> i) & 1 for i in range(n - 1, -1, -1))

    put(0b0010, 4)
    put(len(text), 9)
    for i in range(0, len(text) - 1, 2):
        put(_QR_ALNUM.index(text[i]) * 45 + _QR_ALNUM.index(text[i + 1]), 11)
    if len(text) % 2:
        put(_QR_ALNUM.index(text[-1]), 6)
    put(0, min(4, _QR_DATA_CW * 8 - len(bits)))
    while len(bits) % 8:
        bits.append(0)
    data = bytearray(int("".join(map(str, bits[i : i + 8])), 2) for i in range(0, len(bits), 8))
    while len(data) < _QR_DATA_CW:
        data.append(0xEC if (len(data) - len(bits) // 8) % 2 == 0 else 0x11)

    gen = [1]
    root = 1
    for _ in range(_QR_ECC_CW):
        nxt = [0] * (len(gen) + 1)
        for i, g in enumerate(gen):
            nxt[i] ^= _gf_mul(g, root)
            nxt[i + 1] ^= g
        gen, root = nxt, _gf_mul(root, 2)
    gen = gen[::-1]
    rem = bytearray(_QR_ECC_CW)
    for b in data:
        factor = b ^ rem[0]
        rem = rem[1:] + bytearray(1)
        for i in range(_QR_ECC_CW):
            rem[i] ^= _gf_mul(gen[i + 1], factor)
    codewords = bytes(data) + bytes(rem)

    mod = [[0] * QR_SIZE for _ in range(QR_SIZE)]
    isf = [[False] * QR_SIZE for _ in range(QR_SIZE)]

    def setf(x: int, y: int, v: int) -> None:
        mod[y][x] = int(bool(v))
        isf[y][x] = True

    for cx, cy in ((3, 3), (QR_SIZE - 4, 3), (3, QR_SIZE - 4)):
        for dy in range(-4, 5):
            for dx in range(-4, 5):
                x, y = cx + dx, cy + dy
                if 0 <= x < QR_SIZE and 0 <= y < QR_SIZE:
                    d = max(abs(dx), abs(dy))
                    setf(x, y, d != 2 and d != 4)
    for t in range(8, QR_SIZE - 8):
        setf(t, 6, t % 2 == 0)
        setf(6, t, t % 2 == 0)
    fdata = 1 << 3  # ECC L, mask 0
    frem = fdata
    for _ in range(10):
        frem = (frem << 1) ^ ((frem >> 9) * 0x537)
    fbits = (fdata << 10 | frem) ^ 0x5412

    def fb(i: int) -> int:
        return (fbits >> i) & 1

    for i in range(6):
        setf(8, i, fb(i))
    setf(8, 7, fb(6))
    setf(8, 8, fb(7))
    setf(7, 8, fb(8))
    for i in range(9, 15):
        setf(14 - i, 8, fb(i))
    for i in range(8):
        setf(QR_SIZE - 1 - i, 8, fb(i))
    for i in range(8, 15):
        setf(8, QR_SIZE - 15 + i, fb(i))
    setf(8, QR_SIZE - 8, 1)

    bit = 0
    total = len(codewords) * 8
    right = QR_SIZE - 1
    while right >= 1:
        if right == 6:
            right = 5
        for vert in range(QR_SIZE):
            for j in range(2):
                x = right - j
                upward = ((right + 1) & 2) == 0
                y = QR_SIZE - 1 - vert if upward else vert
                if not isf[y][x] and bit < total:
                    mod[y][x] = (codewords[bit >> 3] >> (7 - (bit & 7))) & 1
                    bit += 1
        right -= 2
    for y in range(QR_SIZE):
        for x in range(QR_SIZE):
            if not isf[y][x] and (x + y) % 2 == 0:
                mod[y][x] ^= 1
    return mod


def display_frame() -> "tuple[_Plane, _Plane]":
    """Compose the mock's panel image: the black plane and the red plane."""
    black, red = _Plane(), _Plane()
    speed = max(0, int(STATE["speed"]))

    black.frame(0, 0, DISP_W, DISP_H)
    # The banner, like the firmware's tricolor path: a solid red band with the
    # title knocked out to paper. One plane -- pixels are never set in both,
    # because that speckles on the glass.
    red.rect(0, 0, DISP_W, 20)
    red.text(5, 4, "GARAGE FAN", scale=2, on=False)

    # Left: the two temperatures, which must DIFFER. Drawing the same value in
    # both slots would let a console that swapped or duplicated them pass.
    inside_f = STATE["outside_f"] + 8
    black.text(6, 26, f"{inside_f:.0f}", scale=4)
    black.text(6, 76, f"{STATE['outside_f']:.0f}", scale=3)

    # Right: the fan, with a red bar whose fill follows the speed, and the
    # console link as a QR in the same spot the firmware draws it.
    black.rect(126, 20, 1, 82)
    black.text(136, 30, str(speed), scale=6)
    qr = qr_v1_encode(f"HTTP://{STATE['ip']}".upper())
    if qr:
        for qy in range(QR_SIZE):
            for qx in range(QR_SIZE):
                if qr[qy][qx]:
                    black.rect(204 + qx * 2, 25 + qy * 2, 2, 2)
    bar_w = DISP_W - 132 - 6
    black.frame(132, 72, bar_w, 10)
    fill = int(round(speed / 12 * (bar_w - 2)))
    if fill > 0:
        red.rect(133, 73, fill, 8)  # red-only, matching the tricolor firmware

    black.rect(0, 104, DISP_W, 1)
    return black, red
