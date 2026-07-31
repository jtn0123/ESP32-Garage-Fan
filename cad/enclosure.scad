// ESP32 eInk Room Node - printable enclosure
// ============================================
// Front shell: frames the 2.13" eInk FeatherWing (Adafruit 4195/4814) with
// the Feather ESP32-S2 stacked behind it. Back tray: cradle for a
// BH-18650-PC4 2x18650 holder, stood off from the electronics with an air
// gap (battery mass = thermal ballast we do NOT want against the BME280).
//
// Everything is parametric; print a test fit, tweak, re-render.
// Render: openscad -o out.stl cad/enclosure.scad
// Preview: openscad -o out.png --imgsize 1200,900 cad/enclosure.scad

// ---- print/fit tuning ----
tol = 0.25;          // XY clearance for press-fit pockets (0.4mm nozzle FDM)
wall = 2.4;          // shell wall thickness (6 perimeters at 0.4)
floor_t = 2.0;       // flat faces

// ---- FeatherWing (display) ----
wing_l = 61.3;       // PCB long edge (display landscape)
wing_w = 40.2;       // PCB short edge
wing_t = 6.7;        // PCB + display glass stack thickness
// visible active area of the 2.13" panel, offset within the wing PCB
disp_l = 48.6;
disp_w = 23.7;
disp_off_x = 0;      // tune from fab print / test fit
disp_off_y = 2.0;    // display sits toward the top edge, away from pin rows

// ---- Feather ESP32-S2 (stacks behind the wing) ----
feather_l = 50.8;
feather_w = 22.86;
stack_h = 14;        // wing underside to feather underside incl. headers

// ---- BH-18650-PC4 dual 18650 holder ----
bh_l = 78.0;
bh_w = 41.5;
bh_h = 21.5;
bh_standoff = 6;     // "a little bit of spacing" behind the electronics

// ---- derived case body ----
case_l = max(wing_l, bh_l) + 2 * (wall + tol);   // holder is the widest part
case_w = max(wing_w, bh_w) + 2 * (wall + tol);
front_depth = wing_t + stack_h + 2;              // display shell cavity
back_depth = bh_h + bh_standoff;                 // battery bay depth

module rounded_box(l, w, h, r = 3) {
  hull()
    for (x = [r, l - r], y = [r, w - r])
      translate([x, y, 0]) cylinder(h = h, r = r, $fn = 48);
}

// ================= FRONT SHELL =================
module front_shell() {
  difference() {
    rounded_box(case_l, case_w, front_depth + floor_t);
    // interior cavity
    translate([wall, wall, floor_t])
      rounded_box(case_l - 2 * wall, case_w - 2 * wall, front_depth + 1, 2);
    // display window through the front face
    translate([(case_l - disp_l) / 2 + disp_off_x,
               (case_w - disp_w) / 2 + disp_off_y, -1])
      cube([disp_l, disp_w, floor_t + 2]);
    // side vents for the BME280 (long edge, both sides)
    for (i = [0:4])
      for (side = [0, 1])
        translate([case_l * 0.25 + i * 8, side * (case_w - wall) - 0.5,
                   floor_t + front_depth * 0.55])
          cube([4, wall + 1, 6]);
    // USB-C cutout, Feather short edge (left wall)
    translate([-1, (case_w - 13) / 2, floor_t + front_depth - stack_h / 2 - 2])
      cube([wall + 2, 13, 8]);
    // BOOT button access hole (page flip), same wall as USB
    translate([-1, (case_w) / 2 + 12, floor_t + front_depth - stack_h / 2])
      rotate([0, 90, 0]) cylinder(h = wall + 2, d = 4, $fn = 24);
  }
  // wing support ledge: corner posts the wing PCB rests on, display-first
  for (x = [(case_l - wing_l) / 2 - tol, (case_l + wing_l) / 2 - 4 + tol],
       y = [(case_w - wing_w) / 2 - tol, (case_w + wing_w) / 2 - 4 + tol])
    translate([x, y, floor_t])
      cube([4, 4, front_depth - stack_h - wing_t]);
}

// ================= BACK TRAY =================
module back_tray() {
  difference() {
    rounded_box(case_l, case_w, back_depth + floor_t);
    // battery holder pocket, snug
    translate([(case_l - bh_l) / 2 - tol, (case_w - bh_w) / 2 - tol,
               floor_t + bh_standoff])
      cube([bh_l + 2 * tol, bh_w + 2 * tol, bh_h + 2]);
    // standoff air gap windows (thermal break + weight)
    for (i = [0:2])
      translate([case_l * 0.2 + i * case_l * 0.22, wall + 4, floor_t - 1])
        rounded_box(case_l * 0.16, case_w - 2 * wall - 8, bh_standoff + 1, 2);
    // wire pass-through into the front shell
    translate([case_l - wall - 8, (case_w - 8) / 2, floor_t + bh_standoff - 4])
      cube([12, 8, 6]);
  }
  // holder retention nubs (leads end stays open for the wires)
  for (y = [(case_w - bh_w) / 2 - tol - 1.6, (case_w + bh_w) / 2 + tol])
    translate([(case_l - bh_l) / 2 + 8, y, floor_t + bh_standoff])
      cube([10, 1.6, bh_h * 0.6]);
}

// ================= LAYOUT =================
part = "assembly";  // "front" | "back" | "assembly"

if (part == "front") front_shell();
else if (part == "back") back_tray();
else {
  front_shell();
  translate([0, 0, -(back_depth + floor_t + 4)]) back_tray();
}
