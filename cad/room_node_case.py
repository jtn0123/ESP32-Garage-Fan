"""ESP32 eInk Room Node — printable enclosure (build123d / OpenCascade).

Real B-rep solids exported as STEP, so this imports into Onshape (or
Fusion/SolidWorks) as editable geometry rather than a dumb mesh.

    venv/bin/python cad/room_node_case.py

Outputs to cad/out/: front_shell + rear_pod as .step and .stl, plus
assembly.step with both parts positioned (for looking at, not printing).

FORM
The awkward fact of this build is that the battery holder (78mm) is much
wider than the display wing (61.3mm). Sizing one box to the holder buries
the little 48.6x23.7mm panel in a huge dead bezel. So the case is two
different footprints: the front shell hugs the wing, and the rear pod
tapers back and outward to the holder's footprint. The taper reads as
deliberate (camera body / monitor stand) instead of as a brick, and it
puts the mass low and behind.

Coordinates: X = long axis, Y = short axis, Z = depth into the case with
the front (display) face at Z=0 and +Z going back.
"""

from pathlib import Path

from build123d import (
    Align,
    Axis,
    Box,
    Cylinder,
    Location,
    Plane,
    Pos,
    Rectangle,
    RigidJoint,
    Rot,
    export_step,
    export_stl,
    fillet,
    loft,
)

# ============================================================================
# DIMENSIONS (mm). Every number is a knob — edit and re-run.
# ============================================================================

# --- Adafruit 2.13" eInk FeatherWing 4195 (tri-color 4814 is identical) ----
WING_L = 61.3
WING_W = 40.2
WING_STACK_T = 6.7          # PCB + eInk glass

DISP_L = 48.6               # visible 250x122 active area
DISP_W = 23.7
DISP_OFF_X = 0.0            # active-area center vs PCB center (+X right)
DISP_OFF_Y = 3.0            # (+Y toward top edge, away from header rows)

# --- Adafruit ESP32-S2 Feather 5303 ---------------------------------------
FEATHER_PCB_T = 1.6
BOARD_GAP = 11.0            # wing underside to Feather topside (headers)
FEATHER_UNDER = 2.5         # JST + solder tails below the Feather

USB_W = 10.0                # USB-C opening (connector ~9mm + clearance)
USB_H = 5.0
BOOT_D = 4.5                # BOOT button access hole

# --- BH-18650-PC4 dual-18650 holder ---------------------------------------
BH_L = 78.0
BH_W = 41.5
BH_H = 21.5

# --- print / fit ----------------------------------------------------------
TOL = 0.25                  # XY clearance on pockets (0.4mm nozzle)
WALL = 2.4                  # shell walls
FACE = 2.0                  # front/back flat faces
POD_STANDOFF = 5.0          # air gap between electronics and cells
TAPER_D = 6.0               # depth of the flare from front footprint to pod
CORNER_R = 4.0
VENT_W = 2.5                # vent slot width
VENT_N = 7                  # slots per side

# --- derived --------------------------------------------------------------
CAVITY_D = WING_STACK_T + BOARD_GAP + FEATHER_PCB_T + FEATHER_UNDER
FRONT_L = WING_L + 2 * (WALL + TOL)
FRONT_W = WING_W + 2 * (WALL + TOL)
FRONT_D = FACE + CAVITY_D
POD_L = BH_L + 2 * (WALL + TOL)
POD_W = BH_W + 2 * (WALL + TOL)
POD_BODY_D = FACE + POD_STANDOFF + BH_H
POD_D = TAPER_D + POD_BODY_D

FEATHER_Z = FACE + WING_STACK_T + BOARD_GAP   # Feather PCB top, for ports

BOT = (Align.CENTER, Align.CENTER, Align.MIN)
OUT = Path(__file__).parent / "out"


def _rounded_rect(length: float, width: float, radius: float):
    """Rounded rectangle sketch, for lofting the taper."""
    return fillet(Rectangle(length, width).vertices(), radius)


def front_shell():
    """Display-side shell: bezel, wing pocket, electronics cavity, ports."""
    body = Box(FRONT_L, FRONT_W, FRONT_D, align=BOT)
    body = fillet(body.edges().filter_by(Axis.Z), CORNER_R)

    # Electronics cavity, open at the back
    body -= Pos(0, 0, FACE) * Box(
        FRONT_L - 2 * WALL, FRONT_W - 2 * WALL, CAVITY_D + 1, align=BOT
    )

    # Wing pocket: the wing drops in glass-first against the front face
    body -= Pos(0, 0, FACE) * Box(
        WING_L + 2 * TOL, WING_W + 2 * TOL, WING_STACK_T, align=BOT
    )

    # Display window through the front face
    body -= Pos(DISP_OFF_X, DISP_OFF_Y, -0.5) * Box(
        DISP_L, DISP_W, FACE + 1, align=BOT
    )

    # USB-C on the -X wall at the Feather's board level
    body -= Pos(-FRONT_L / 2 - 1, 0, FEATHER_Z) * Box(
        WALL + 4, USB_W, USB_H, align=(Align.MIN, Align.CENTER, Align.MIN)
    )

    # BOOT button access, same wall, offset in Y
    body -= (
        Pos(-FRONT_L / 2 - 1, 13, FEATHER_Z + 2)
        * Rot(0, 90, 0)
        * Cylinder(BOOT_D / 2, WALL + 4, align=BOT)
    )

    # Side vents for the BME280, both long walls at board level
    for sy in (-1, 1):
        for i in range(VENT_N):
            x = (i - (VENT_N - 1) / 2) * (VENT_W * 2.6)
            body -= Pos(x, sy * (FRONT_W / 2), FEATHER_Z - 4) * Box(
                VENT_W, WALL + 4, 9, align=(Align.CENTER, Align.CENTER, Align.MIN)
            )

    return body


def rear_pod():
    """Battery pod: flares from the front footprint out to the holder's."""
    # Taper section: front footprint -> pod footprint
    taper = loft(
        [
            Plane.XY * _rounded_rect(FRONT_L, FRONT_W, CORNER_R),
            Plane.XY.offset(TAPER_D) * _rounded_rect(POD_L, POD_W, CORNER_R),
        ]
    )
    body = taper + Pos(0, 0, TAPER_D) * Box(POD_L, POD_W, POD_BODY_D, align=BOT)
    body = fillet(body.edges().filter_by(Axis.Z), CORNER_R * 0.9)

    # Holder pocket, opening toward the back face
    body -= Pos(0, 0, POD_D - BH_H) * Box(
        BH_L + 2 * TOL, BH_W + 2 * TOL, BH_H + 1, align=BOT
    )

    # Air-gap windows through the standoff shelf (thermal break + weight)
    for i in range(3):
        x = (i - 1) * (POD_L * 0.26)
        body -= Pos(x, 0, -0.5) * Box(
            POD_L * 0.17, POD_W - 2 * WALL - 8, TAPER_D + POD_STANDOFF, align=BOT
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
    print(f"total depth {FRONT_D + POD_D:.1f} mm")
    bez_x = (FRONT_L - DISP_L) / 2
    print(f"bezel      {bez_x:.1f} mm sides")


if __name__ == "__main__":
    main()
