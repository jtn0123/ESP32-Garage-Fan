"""Room Node — "desk clock" variant.

Alternative to the battery-behind layout in room_node_case.py. Here the
2x18650 holder lies flat in a base and the display head stands on it,
leaning back ~12 degrees. The point is proportion: the head only has to
be as big as the wing, the base absorbs the holder's extra 17mm of
length, and the cells end up at the bottom where they make the thing
stable instead of tippy.

    venv/bin/python cad/room_node_clock.py

All dimensions and the port/vent cutting live in room_node_case.py — this
file is layout only, so the two variants can never drift apart.
"""

from pathlib import Path

from build123d import (
    Align,
    Axis,
    Box,
    Pos,
    Rot,
    export_step,
    export_stl,
    fillet,
)

from room_node_case import (  # noqa: E402  (shared dimensions + helpers)
    BH_H,
    BH_L,
    BH_W,
    CAVITY_D,
    CORNER_R,
    DISP_BLEED,
    DISP_L,
    DISP_OFF_X,
    DISP_OFF_Y,
    DISP_W,
    FACE,
    TOL,
    WALL,
    WING_L,
    WING_POCKET_D,
    WING_W,
    _ports,
    _vents,
)

TILT = 12.0            # degrees the head leans back

HEAD_L = WING_L + 2 * (WALL + TOL)
HEAD_W = WING_W + 2 * (WALL + TOL)   # becomes the head's HEIGHT stood up
HEAD_D = FACE + CAVITY_D

BASE_L = BH_L + 2 * (WALL + TOL)
BASE_W = BH_W + 2 * (WALL + TOL)
BASE_H = FACE + BH_H + FACE

BOT = (Align.CENTER, Align.CENTER, Align.MIN)
OUT = Path(__file__).parent / "out"


def head():
    """Display head — same internals as the case variant, stood upright."""
    body = Box(HEAD_L, HEAD_W, HEAD_D, align=BOT)
    body = fillet(body.edges().filter_by(Axis.Z), CORNER_R)

    body -= Pos(0, 0, FACE) * Box(
        HEAD_L - 2 * WALL, HEAD_W - 2 * WALL, CAVITY_D + 1, align=BOT
    )
    body -= Pos(0, 0, FACE) * Box(
        WING_L + 2 * TOL, WING_W + 2 * TOL, WING_POCKET_D, align=BOT
    )
    body -= Pos(DISP_OFF_X, DISP_OFF_Y, -0.5) * Box(
        DISP_L + 2 * DISP_BLEED, DISP_W + 2 * DISP_BLEED, FACE + 1, align=BOT
    )
    body = _ports(body, HEAD_L / 2, HEAD_W / 2)
    body = _vents(body, HEAD_W / 2)
    return body


def base():
    """Battery base: holder pocket, wire route up into the head."""
    body = Box(BASE_L, BASE_W, BASE_H, align=BOT)
    body = fillet(body.edges().filter_by(Axis.Z), CORNER_R)

    # Holder pocket, loaded from underneath
    body -= Pos(0, 0, -0.5) * Box(
        BH_L + 2 * TOL, BH_W + 2 * TOL, BH_H + 0.5, align=BOT
    )
    # Wire route up through the back of the base into the head
    body -= Pos(0, BASE_W / 2 - 10, BASE_H - FACE - 0.5) * Box(
        12, 9, FACE + 1, align=BOT
    )
    return body


def main() -> None:
    OUT.mkdir(exist_ok=True)
    h = head()
    b = base()

    export_step(h, str(OUT / "clock_head.step"))
    export_stl(h, str(OUT / "clock_head.stl"))
    export_step(b, str(OUT / "clock_base.step"))
    export_stl(b, str(OUT / "clock_base.stl"))

    # Stand the head up so the display faces -Y, leaning back by TILT, then
    # seat it from its own bounding box: rotation puts the origin somewhere
    # unhelpful, and hand-computed offsets left it floating above the base
    # like a carry handle.
    stood = Rot(-90 - TILT, 0, 0) * h
    bb = stood.bounding_box()
    stood = Pos(
        -bb.center().X,
        (BASE_W / 2 - WALL) - bb.max.Y,   # back face flush inside the base
        BASE_H - bb.min.Z,                # bottom edge seated on the base top
    ) * stood

    asm = b + stood
    export_step(asm, str(OUT / "clock_assembly.step"))
    export_stl(asm, str(OUT / "clock_assembly.stl"))

    print(f"base  {BASE_L:.1f} x {BASE_W:.1f} x {BASE_H:.1f} mm")
    print(f"head  {HEAD_L:.1f} wide x {HEAD_W:.1f} tall x {HEAD_D:.1f} deep")
    print(f"overall height ~{BASE_H + HEAD_W:.0f} mm, tilt {TILT:.0f} deg")


if __name__ == "__main__":
    main()
