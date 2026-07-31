"""ESP32 eInk Room Node — printable enclosure (build123d / OpenCascade).

Real B-rep solids exported as STEP, so this imports into Onshape (or
Fusion/SolidWorks) as editable geometry rather than a dumb mesh.

    venv/bin/python cad/room_node_case.py

Outputs to cad/out/: front_shell + rear_pod as .step and .stl, plus
assembly.step with both parts positioned (for looking at, not printing).

FORM
The battery holder (77.7mm) is much wider than the display wing (61mm),
so a single-footprint box strands the little 48.6x23.7mm panel in a huge
dead bezel. Instead the front shell hugs the wing and the rear pod lofts
outward to the holder's footprint, so the silhouette steps rather than
reading as one brick. room_node_clock.py is the alternative layout.

FRAME
Looking AT the display: +X right, +Y up, +Z back into the case. The front
(display) face is Z=0. The USB-C end of the Feather is at -X, and the
microSD slot is on that same end — see MICROSD below, it is the single
most annoying layout constraint in this build.
"""

from pathlib import Path

from build123d import (
    Align,
    Axis,
    Box,
    Cylinder,
    Plane,
    Pos,
    Rectangle,
    Rot,
    export_step,
    export_stl,
    fillet,
    loft,
)

# ============================================================================
# DIMENSIONS (mm)
#
# [CAD]  = read out of Adafruit's published Eagle .brd files
# [DS]   = component datasheet
# [PUB]  = Adafruit product page
# [EST]  = estimated, verify on hardware before committing to a print
# ============================================================================

# --- Adafruit 2.13" eInk FeatherWing 4195 / 4814 (mechanically identical) --
# The PCB is a "dog-bone": a 31.37mm waist with 2.54mm corner ears carrying
# the mounting holes. The pocket below uses the bounding box, so the four
# ears locate the board and the waist simply has air around it.
WING_L = 60.97  # [CAD] bounding box, long axis
WING_W = 40.51  # [CAD] bounding box across the mounting ears
WING_WAIST_W = 31.37  # [CAD] main body width
WING_GLASS_T = 1.05  # [DS]  eInk glass
WING_PCB_T = 1.6  # [EST] Adafruit standard
WING_BACK_T = 1.9  # [EST] microSD socket standing off the back
WING_STACK_T = 6.7  # [PUB] overall; does not decompose cleanly, see
#                                   research note — measure before trusting

# Visible 250x122 active area. [CAD]+[DS]: begins 3.71mm from the USB-C end,
# ends 8.71mm short of the far end, and is centred across the short axis.
DISP_L = 48.55
DISP_W = 23.70
DISP_FROM_USB_END = 3.71
DISP_OFF_X = -(WING_L / 2) + DISP_FROM_USB_END + DISP_L / 2  # = -2.5
DISP_OFF_Y = 0.0  # centred (8.35 vs 8.47mm to the two long edges)
DISP_BLEED = 1.0  # extra window all round: the glass is adhered,
#                             not registered to the mounting holes

# Wing mounting holes [CAD]: 2.54mm in from each bounding-box corner
WING_HOLE_D = 2.5
WING_HOLE_PITCH_X = 55.88
WING_HOLE_PITCH_Y = 35.43

# microSD on the wing's underside [CAD]. The slot faces the USB-C end and
# needs ~3.5mm of clear air beyond the PCB edge to insert/eject a card —
# which puts card access and USB-C access on the SAME wall.
SD_SLOT_W = 14.0  # card 11mm + clearance
SD_SLOT_H = 3.0
SD_CLEAR = 4.0

# --- Adafruit ESP32-S2 Feather 5303 ---------------------------------------
FEATHER_L = 50.80  # [CAD]
FEATHER_W = 22.86  # [CAD]
FEATHER_PCB_T = 1.6  # [EST]
FEATHER_UNDER = 2.5  # [EST] header pin tails; nothing else is on the
#                                   bottom side [CAD]
BOARD_GAP = 11.0  # [DER] 8.5mm socket + 2.54mm male spacer; the
#                                   1.9mm microSD hangs into this gap fine

# Component positions, measured from the Feather's USB-C end [CAD].
# The Feather sits flush with the wing at the USB-C end, so these convert
# straight into the wing frame.
BME280_FROM_USB = 24.77  # [CAD] mid-board — vent here
USB_FROM_USB = 0.0  # [CAD] protrudes 1.14mm past the PCB edge
# The receptacle is 8.94 x ~3.2mm, but the opening has to swallow a CABLE
# OVERMOLD, which on a typical USB-C lead is ~11-12 x 6-7mm. Sizing the hole
# to the receptacle means the plug will not seat.
USB_W = 12.5  # [DER] cable overmold clearance
USB_H = 7.5  # [DER]
USB_BODY_H = 3.2  # [EST] receptacle height above the PCB
BOOT_FROM_USB = 20.96  # [CAD] centre of the BOOT tactile switch
BOOT_FROM_ROW_EDGE = 5.21  # [CAD] from the 16-pin-row long edge
BOOT_D = 4.0  # poke-hole: the switch faces UP into the board
#                             gap and is covered by the wing, so it can only
#                             be reached with a thin tool through the wall

# --- BH-18650-PC4 dual-18650 holder ---------------------------------------
# [DS] MPD BK-18650-PC4 drawing; the Amazon clone shares the mould.
BH_L = 77.70
BH_W = 40.21
BH_H = 21.54
BH_HOLE_PITCH_X = 55.62
BH_HOLE_PITCH_Y = 19.30
BH_HOLE_D = 3.2  # [EST] clone's own hole; drawing gives the
#                                   recommended PCB clearance hole

# --- print / fit ----------------------------------------------------------
TOL = 0.25  # XY clearance on pockets (0.4mm nozzle)
WALL = 2.4  # shell walls
FACE = 2.0  # front/back flat faces
POD_STANDOFF = 5.0  # air gap between electronics and cells
TAPER_D = 6.0  # flare depth from front footprint out to the pod
CORNER_R = 4.0
VENT_W = 2.5
VENT_N = 7

# --- assembly ------------------------------------------------------------
# The wing very nearly fills the front shell, so there is no room for screw
# bosses in the shell's corners. Instead the bosses live ON the wing's own
# mounting-hole pattern: a slim post locates the wing, and the boss behind
# it receives an M2.5 self-tapper driven in from the battery pocket. The
# pattern sits clear of both the active area and the Feather's outline.
POST_D = 2.3  # slip fit through the wing's 2.5mm holes
BOSS_D = 6.0
BOSS_PILOT_D = 2.1  # M2.5 self-tapping into printed plastic
SCREW_CLEAR_D = 2.8
SCREW_HEAD_D = 5.4
SCREW_HEAD_DEPTH = 2.4

# --- derived --------------------------------------------------------------
WING_POCKET_D = WING_GLASS_T + WING_PCB_T  # what the pocket captures
CAVITY_D = WING_POCKET_D + BOARD_GAP + FEATHER_PCB_T + FEATHER_UNDER
FRONT_L = WING_L + 2 * (WALL + TOL)
FRONT_W = WING_W + 2 * (WALL + TOL)
FRONT_D = FACE + CAVITY_D
POD_L = BH_L + 2 * (WALL + TOL)
POD_W = BH_W + 2 * (WALL + TOL)
POD_BODY_D = FACE + POD_STANDOFF + BH_H
POD_D = TAPER_D + POD_BODY_D

# Z of the Feather's PCB top face, used to place every port
FEATHER_Z = FACE + WING_POCKET_D + BOARD_GAP
# X of the wing's USB-C end edge, and the conversion for Feather features
USB_END_X = -WING_L / 2
# Y of the Feather's 16-pin-row long edge in the wing frame: the Feather is
# centred across the wing's short axis
FEATHER_ROW_EDGE_Y = -FEATHER_W / 2

BOT = (Align.CENTER, Align.CENTER, Align.MIN)
OUT = Path(__file__).parent / "out"


def _rounded_rect(length: float, width: float, radius: float):
    """Rounded rectangle sketch, for lofting the taper."""
    return fillet(Rectangle(length, width).vertices(), radius)


def _ports(body, half_l: float, half_w: float):
    """Cut USB-C, microSD and the BOOT poke-hole into the -X wall.

    Z matters here and got it wrong once: the Feather stacks UNDER the wing
    with its component side facing the display, so everything mounted on it
    (USB-C, buttons, BME280) lives between FEATHER_Z and the display, i.e.
    toward -Z. Cutting +Z from FEATHER_Z opens holes into the empty space
    behind the board instead.
    """
    # USB-C: sits on the Feather's top face, so it occupies -Z from there.
    # Opening is sized for the cable overmold, not the receptacle.
    body -= Pos(-half_l - 1, 0, FEATHER_Z - USB_BODY_H - (USB_H - USB_BODY_H) / 2) * Box(
        WALL + 6, USB_W, USB_H, align=(Align.MIN, Align.CENTER, Align.MIN)
    )
    # microSD ejects from the wing's underside toward this same wall
    body -= Pos(-half_l - 1, 0, FACE + WING_POCKET_D) * Box(
        WALL + 5 + SD_CLEAR,
        SD_SLOT_W,
        SD_SLOT_H,
        align=(Align.MIN, Align.CENTER, Align.MIN),
    )
    # BOOT poke-hole, aimed into the board gap at the switch centre
    boot_y = FEATHER_ROW_EDGE_Y + BOOT_FROM_ROW_EDGE
    body -= (
        Pos(-half_l - 1, boot_y, FEATHER_Z - 1.6)
        * Rot(0, 90, 0)
        * Cylinder(BOOT_D / 2, WALL + 6, align=BOT)
    )
    return body


def _boss_xy():
    """The wing's mounting-hole pattern — also the case's screw pattern."""
    return [
        (sx * WING_HOLE_PITCH_X / 2, sy * WING_HOLE_PITCH_Y / 2) for sx in (-1, 1) for sy in (-1, 1)
    ]


def _add_bosses(body, depth: float):
    """Locating posts through the wing + screw bosses behind it."""
    for x, y in _boss_xy():
        # boss body, behind the wing
        body += Pos(x, y, FACE + WING_POCKET_D) * Cylinder(
            BOSS_D / 2, depth - FACE - WING_POCKET_D, align=BOT
        )
        # slim post through the wing's own mounting hole
        body += Pos(x, y, FACE) * Cylinder(POST_D / 2, WING_POCKET_D, align=BOT)
        # pilot hole for the self-tapper, from the back
        body -= Pos(x, y, FACE + WING_POCKET_D) * Cylinder(
            BOSS_PILOT_D / 2, depth - FACE - WING_POCKET_D + 1, align=BOT
        )
    return body


def _vents(body, half_w: float):
    """Slots on both long walls, centred on the BME280."""
    bme_x = USB_END_X + BME280_FROM_USB
    for sy in (-1, 1):
        for i in range(VENT_N):
            x = bme_x + (i - (VENT_N - 1) / 2) * (VENT_W * 2.6)
            body -= Pos(x, sy * half_w, FEATHER_Z - 4) * Box(
                VENT_W, WALL + 4, 9, align=(Align.CENTER, Align.CENTER, Align.MIN)
            )
    return body


def front_shell():
    """Display-side shell: bezel, wing pocket, electronics cavity, ports."""
    body = Box(FRONT_L, FRONT_W, FRONT_D, align=BOT)
    body = fillet(body.edges().filter_by(Axis.Z), CORNER_R)

    # Electronics cavity, open at the back
    body -= Pos(0, 0, FACE) * Box(FRONT_L - 2 * WALL, FRONT_W - 2 * WALL, CAVITY_D + 1, align=BOT)
    # Wing pocket: the wing drops in glass-first against the front face.
    # Bounding box, so the four corner ears locate it.
    body -= Pos(0, 0, FACE) * Box(WING_L + 2 * TOL, WING_W + 2 * TOL, WING_POCKET_D, align=BOT)
    # Display window
    body -= Pos(DISP_OFF_X, DISP_OFF_Y, -0.5) * Box(
        DISP_L + 2 * DISP_BLEED, DISP_W + 2 * DISP_BLEED, FACE + 1, align=BOT
    )

    body = _add_bosses(body, FRONT_D)
    body = _ports(body, FRONT_L / 2, FRONT_W / 2)
    body = _vents(body, FRONT_W / 2)
    return body


def rear_pod():
    """Battery pod: flares from the front footprint out to the holder's."""
    taper = loft(
        [
            Plane.XY * _rounded_rect(FRONT_L, FRONT_W, CORNER_R),
            Plane.XY.offset(TAPER_D) * _rounded_rect(POD_L, POD_W, CORNER_R),
        ]
    )
    body = taper + Pos(0, 0, TAPER_D) * Box(POD_L, POD_W, POD_BODY_D, align=BOT)
    body = fillet(body.edges().filter_by(Axis.Z), CORNER_R * 0.9)

    # Holder pocket, opening toward the back face
    body -= Pos(0, 0, POD_D - BH_H) * Box(BH_L + 2 * TOL, BH_W + 2 * TOL, BH_H + 1, align=BOT)
    # Air-gap windows through the standoff shelf (thermal break + weight)
    for i in range(3):
        x = (i - 1) * (POD_L * 0.26)
        body -= Pos(x, 0, -0.5) * Box(
            POD_L * 0.17, POD_W - 2 * WALL - 8, TAPER_D + POD_STANDOFF, align=BOT
        )
    # Wire pass-through from the cells to the electronics bay
    body -= Pos(POD_L / 2 - 12, 0, TAPER_D + POD_STANDOFF - 6) * Box(14, 9, 7, align=BOT)
    # Screw holes onto the front shell's bosses, driven from inside the
    # battery pocket (fit the cells last). Counterbored so the heads sit
    # below the pocket floor and the holder still seats flat.
    floor_z = POD_D - BH_H
    for x, y in _boss_xy():
        body -= Pos(x, y, -1) * Cylinder(SCREW_CLEAR_D / 2, floor_z + 2, align=BOT)
        body -= Pos(x, y, floor_z - SCREW_HEAD_DEPTH) * Cylinder(
            SCREW_HEAD_D / 2, SCREW_HEAD_DEPTH + 1, align=BOT
        )
    return body


def main() -> None:
    OUT.mkdir(exist_ok=True)
    front = front_shell()
    rear = rear_pod()

    export_step(front, str(OUT / "front_shell.step"))
    export_stl(front, str(OUT / "front_shell.stl"))
    export_step(rear, str(OUT / "rear_pod.step"))
    export_stl(rear, str(OUT / "rear_pod.stl"))

    asm = front + Pos(0, 0, FRONT_D) * rear
    export_step(asm, str(OUT / "assembly.step"))
    export_stl(asm, str(OUT / "assembly.stl"))

    print(f"front face {FRONT_L:.1f} x {FRONT_W:.1f} mm  ({FRONT_D:.1f} deep)")
    print(f"rear pod   {POD_L:.1f} x {POD_W:.1f} mm  ({POD_D:.1f} deep)")
    print(f"total      {FRONT_D + POD_D:.1f} mm deep")
    print(
        f"window     {DISP_L + 2 * DISP_BLEED:.1f} x {DISP_W + 2 * DISP_BLEED:.1f}"
        f" at x={DISP_OFF_X:+.1f}"
    )
    print(
        f"bezel      {(FRONT_L - DISP_L) / 2 - DISP_BLEED:.1f} / "
        f"{(FRONT_W - DISP_W) / 2 - DISP_BLEED:.1f} mm"
    )


if __name__ == "__main__":
    main()
