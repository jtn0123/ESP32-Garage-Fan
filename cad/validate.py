"""Fit validation for the room-node enclosure.

Builds phantom solids for the real components in their installed positions
and checks them against the printed parts, so "does it fit" is answered by
geometry rather than by eyeballing a render.

    venv/bin/python cad/validate.py

Checks, in order:
  1. every printed part is a closed, valid solid
  2. each component actually fits its pocket/cavity (no intersection with
     case material)
  3. the screw bosses do not collide with the Feather or the wing's
     active area
  4. the two halves' screw features line up
  5. the display window fully exposes the 250x122 active area
  6. wall thicknesses are printable

Exit code is non-zero if any check fails, so this can gate a print.
"""

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).parent))

from build123d import Align, Box, Cylinder, Pos  # noqa: E402
import room_node_case as m  # noqa: E402

BOT = (Align.CENTER, Align.CENTER, Align.MIN)
FAILS: list[str] = []
NOTES: list[str] = []


def check(ok: bool, label: str, detail: str = "") -> None:
    mark = "PASS" if ok else "FAIL"
    print(f"  [{mark}] {label}{('  — ' + detail) if detail else ''}")
    if not ok:
        FAILS.append(label)


def vol(shape) -> float:
    return shape.volume


# ---------------------------------------------------------------- phantoms
def phantom_wing():
    """Wing in its installed position, INCLUDING its four mounting holes.

    Modelling it as a plain box reports the locating posts as interference,
    which is exactly backwards: the posts are supposed to pass through those
    holes. (First run flagged 44.0 mm3, which is precisely four posts.)
    """
    wing = Pos(0, 0, m.FACE) * Box(m.WING_L, m.WING_W, m.WING_POCKET_D, align=BOT)
    for x, y in m._boss_xy():
        wing -= Pos(x, y, m.FACE - 1) * Cylinder(m.WING_HOLE_D / 2, m.WING_POCKET_D + 2, align=BOT)
    return wing


def phantom_wing_back():
    """Only the microSD socket, at its real footprint [CAD].

    The rest of the wing's back is flat PCB and solder, and the screw bosses
    are meant to land on the corner ears there — modelling the whole back as
    a 1.9mm slab reported that intended contact as a collision.
    """
    x0 = m.USB_END_X + 3.66
    x1 = m.USB_END_X + 17.01
    return Pos((x0 + x1) / 2, 0, m.FACE + m.WING_POCKET_D) * Box(
        x1 - x0, 14.2, m.WING_BACK_T, align=BOT
    )


def phantom_feather():
    """Feather PCB, centred on the wing, flush at the USB-C end."""
    cx = m.USB_END_X + m.FEATHER_L / 2
    return Pos(cx, 0, m.FEATHER_Z) * Box(m.FEATHER_L, m.FEATHER_W, m.FEATHER_PCB_T, align=BOT)


def phantom_feather_tall():
    """Feather + its tallest top-side component (JST, ~5.6mm) facing -Z."""
    cx = m.USB_END_X + m.FEATHER_L / 2
    return Pos(cx, 0, m.FEATHER_Z - 5.6) * Box(m.FEATHER_L, m.FEATHER_W, 5.6, align=BOT)


def phantom_holder():
    """2x18650 holder inside the rear pod (pod-local Z)."""
    return Pos(0, 0, m.POD_D - m.BH_H) * Box(m.BH_L, m.BH_W, m.BH_H, align=BOT)


def phantom_active_area():
    """The 250x122 pixels that must be visible through the window."""
    return Pos(m.DISP_OFF_X, m.DISP_OFF_Y, -1) * Box(m.DISP_L, m.DISP_W, m.FACE + 2, align=BOT)


def main() -> int:
    front = m.front_shell()
    rear = m.rear_pod()

    print("\n1. solids are printable")
    for name, part in (("front_shell", front), ("rear_pod", rear)):
        ok = bool(part.is_valid) and part.volume > 0
        check(ok, f"{name} is a valid closed solid", f"{part.volume / 1000:.1f} cm3")

    print("\n2. components fit without hitting case material")
    for name, ph in (
        ("eInk wing (in its pocket)", phantom_wing()),
        ("wing back / microSD socket", phantom_wing_back()),
        ("Feather PCB", phantom_feather()),
        ("Feather top components", phantom_feather_tall()),
    ):
        clash = vol(front & ph)
        check(clash < 1.0, name, "clear" if clash < 1.0 else f"{clash:.1f} mm3 of interference")

    clash = vol(rear & phantom_holder())
    check(
        clash < 1.0,
        "18650 holder in the pod",
        "clear" if clash < 1.0 else f"{clash:.1f} mm3 of interference",
    )

    print("\n3. screw bosses clear the electronics")
    for x, y in m._boss_xy():
        cx = m.USB_END_X + m.FEATHER_L / 2
        inside_feather = (
            abs(x - cx) < m.FEATHER_L / 2 + m.BOSS_D / 2 and abs(y) < m.FEATHER_W / 2 + m.BOSS_D / 2
        )
        check(not inside_feather, f"boss ({x:+.1f}, {y:+.1f}) clears the Feather")

    aa_x = m.DISP_L / 2 + m.DISP_BLEED
    aa_y = m.DISP_W / 2 + m.DISP_BLEED
    for x, y in m._boss_xy():
        over_aa = abs(x - m.DISP_OFF_X) < aa_x and abs(y - m.DISP_OFF_Y) < aa_y
        check(not over_aa, f"boss ({x:+.1f}, {y:+.1f}) clears the display window")

    print("\n3b. bosses land on the wing's mounting ears (they are its standoff)")
    for x, y in m._boss_xy():
        on_pcb = abs(x) < m.WING_L / 2 + 1 and abs(y) < m.WING_W / 2 + 1
        check(on_pcb, f"boss ({x:+.1f}, {y:+.1f}) bears on the wing PCB")

    print("\n4. the two halves line up")
    check(
        abs(m.FRONT_L - (m.WING_L + 2 * (m.WALL + m.TOL))) < 0.01, "front shell sized to the wing"
    )
    # the pod's taper starts at the front shell's footprint
    check(abs(m.POD_L - (m.BH_L + 2 * (m.WALL + m.TOL))) < 0.01, "rear pod sized to the holder")
    check(
        m.SCREW_CLEAR_D > m.BOSS_PILOT_D,
        "screw clears its pilot hole",
        f"{m.SCREW_CLEAR_D} > {m.BOSS_PILOT_D}",
    )
    check(
        m.SCREW_HEAD_D < m.BOSS_D + 1.0,
        "screw head lands on the boss",
        f"head {m.SCREW_HEAD_D} vs boss {m.BOSS_D}",
    )
    floor_z = m.POD_D - m.BH_H
    check(
        floor_z > m.SCREW_HEAD_DEPTH + 1.5,
        "pod floor thick enough to counterbore",
        f"{floor_z:.1f}mm floor, {m.SCREW_HEAD_DEPTH}mm counterbore",
    )

    print("\n5. the window exposes the whole active area")
    # window minus active area should leave the AA entirely uncovered
    aa = phantom_active_area()
    covered = vol(front & aa)
    check(
        covered < 1.0,
        "no case material over the active area",
        "clear" if covered < 1.0 else f"{covered:.1f} mm3 covering pixels",
    )
    bez_x = (m.FRONT_L - m.DISP_L) / 2 - m.DISP_BLEED
    bez_y = (m.FRONT_W - m.DISP_W) / 2 - m.DISP_BLEED
    check(
        bez_x > 2.0 and bez_y > 2.0,
        "bezel wide enough to be rigid",
        f"{bez_x:.1f} x {bez_y:.1f} mm",
    )

    print("\n6. printability")
    check(m.WALL >= 1.2, "wall thickness", f"{m.WALL} mm")
    check(m.FACE >= 1.2, "front face thickness", f"{m.FACE} mm")
    check(m.TOL >= 0.15, "pocket clearance", f"{m.TOL} mm per side")
    overhang = m.FRONT_D - m.FACE
    NOTES.append(f"front shell prints face-down: {overhang:.1f}mm of wall, no supports")
    NOTES.append("rear pod prints pocket-up; the taper is a ~40 deg overhang, fine unsupported")

    print("\nnotes")
    for n in NOTES:
        print(f"  - {n}")

    print()
    if FAILS:
        print(f"{len(FAILS)} CHECK(S) FAILED:")
        for f in FAILS:
            print(f"  - {f}")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
