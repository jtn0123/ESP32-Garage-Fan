ESP32 eInk Room Node — 3D printable enclosure
==============================================

STATUS: geometry-validated, NOT hardware-validated.
        Print the test piece first (see below). Do not print the whole
        set until the wing actually drops into the pocket.


WHAT TO PRINT
-------------
Pick ONE layout. Both hold the same electronics.

  A) "battery behind"  — 83 x 46 x 54 mm brick
     print-these-STL/front_shell.stl
     print-these-STL/rear_pod.stl

  B) "desk clock"      — 83 x 46 x 71 mm, display leans back 12 deg
     print-these-STL/clock_head.stl
     print-these-STL/clock_base.stl

Anything named *assembly* in editable-STEP/ is both halves positioned so
you can look at them. It is NOT printable — it would come out fused.


PRINT SETTINGS
--------------
  nozzle       0.4 mm  (pocket clearances assume this)
  layer        0.2 mm
  walls        4+ (case walls are 2.4 mm)
  infill       20-30%
  supports     NONE needed if oriented as below
  material     PLA or PETG. PETG if it sits in sun or a warm room.

  front_shell / clock_head : display face DOWN on the plate.
                             Gives a clean bezel face, no supports.
  rear_pod / clock_base    : battery pocket UP.
                             The taper is a ~40 deg overhang, fine.


HARDWARE NEEDED
---------------
  4x M2.5 self-tapping screws, 8-10 mm long
     (they cut their own thread into the printed bosses)


ASSEMBLY ORDER
--------------
  1. eInk FeatherWing onto the four locating posts, glass toward the
     display window. The posts pass through the wing's own mounting holes.
  2. Feather ESP32-S2 plugged into the wing's headers underneath.
  3. Battery holder wires routed through the pass-through.
  4. Mate the halves, drive 4 screws from INSIDE the battery pocket.
  5. 18650 cells in LAST — the screw heads are counterbored below the
     pocket floor so the holder still seats flat.


WHAT WAS ACTUALLY VALIDATED  (see VALIDATION-REPORT.txt)
--------------------------------------------------------
26 automated geometry checks, all passing:
  - both parts are closed, valid solids
  - wing, Feather, Feather's tall components and the 18650 holder all fit
    their cavities with zero interference
  - screw bosses clear the Feather outline and the display window, and do
    bear on the wing's mounting ears
  - the two halves' screw features line up; pod floor is thick enough to
    counterbore
  - no case material covers any of the 250x122 active area
  - wall/face thickness and pocket clearance are printable

Component dimensions came from Adafruit's published Eagle CAD files and
component datasheets, not from guesswork.


WHAT IS **NOT** VALIDATED — read this before printing everything
----------------------------------------------------------------
Nothing has touched real hardware. Four numbers are still estimates:

  1. USB-C connector HEIGHT (assumed 3.2 mm). Could not find a datasheet
     for Adafruit's exact part. The opening is generously sized (12.5 x
     7.5 mm) to absorb this, but verify.

  2. The wing's overall thickness. Adafruit publishes 6.7 mm, but the
     layers I can account for (1.05 glass + 1.6 PCB + 1.9 microSD socket)
     only reach ~4.9 mm. Something on its back is taller than the CAD
     shows. This affects internal clearance.

  3. Both PCB thicknesses (assumed 1.6 mm, Adafruit's standard).

  4. Where the wires exit YOUR battery holder. The datasheet version has
     solder pins, not leads. The wire pass-through is on the +X end —
     if yours exits elsewhere, that cutout is in the wrong place.

  5. Board-to-board spacing is derived (8.5 mm socket + 2.54 mm spacer =
     11 mm) and not measured on your actual stack.


DO THIS FIRST
-------------
Print ONLY front_shell.stl (or clock_head.stl). ~1.5 hours.
Then check, with real parts in hand:
  - does the wing drop into the pocket and sit flat?
  - do the four posts line up with its mounting holes?
  - is the display window centred on the visible panel area?
  - does a USB-C cable reach the port and seat fully?
  - can you reach BOOT through the poke-hole with a paperclip?

Tell me what is off and I will adjust — every dimension is a named
constant in cad/room_node_case.py, so corrections are one-line changes
and a re-run, not a remodel.
