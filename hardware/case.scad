// Security Dev enclosure
// ESP32-S3-DevKitC-1 + 1.3" SH1106 OLED + INMP441 MEMS microphone
//
// Export STLs:
//   openscad -o case_base.stl -D 'part="base"' case.scad
//   openscad -o case_lid.stl  -D 'part="lid"'  case.scad
//
// VERIFY BEFORE PRINTING (marked [V] below): the OLED mounting hole spacing and
// the USB port positions differ between vendors and board revisions.

part = "both";          // "base" | "lid" | "both"
$fn = 48;

/* ---------- shell ---------- */
wall     = 2.4;         // 6 perimeters at a 0.4 mm nozzle
floor_t  = 2.0;
lid_t    = 2.4;
inner_x  = 66.0;        // DevKitC-1 length + clearance
inner_y  = 44.0;        // DevKitC-1 width + microphone strip
inner_z  = 18.0;        // floor to lid underside
fillet   = 3.0;
clr      = 0.3;         // print clearance on mating features

/* ---------- ESP32-S3-DevKitC-1 ---------- */
// This board has no standard mounting holes, so it is captured between ribs
// and rests on four pads rather than being screwed down.
dk_x        = 63.0;
dk_y        = 25.5;
dk_t        = 1.6;
dk_standoff = 4.0;      // clearance underneath for the header pin tails
dk_off_y    = -8.0;     // shifted off centre; the mic lives in the freed strip
rib_h       = 3.0;      // rib height above the PCB top face
pad_d       = 5.0;

/* ---------- USB-C, on the -X end wall ---------- */
usb_w   = 9.6;          // [V] port opening width
usb_h   = 4.4;
usb_gap = 12.4;         // [V] centre-to-centre of the two ports

/* ---------- 1.3" OLED, mounted to the lid ---------- */
ol_x       = 35.5;
ol_y       = 33.5;
ol_win_x   = 30.4;      // active area 29.42 + margin
ol_win_y   = 16.2;      // active area 14.73 + margin
ol_hole_dx = 30.5;      // [V] mounting hole spacing, X
ol_hole_dy = 28.5;      // [V] mounting hole spacing, Y
ol_hole_d  = 1.8;       // M2 self-tapping pilot
ol_boss_d  = 4.5;
ol_boss_h  = 3.2;
ol_off_x   = 8.0;       // sits over the module end of the devkit
ol_off_y   = -8.0;

/* ---------- INMP441 ---------- */
mic_x      = 15.3;
mic_y      = 13.0;
mic_off_x  = -20.0;
mic_off_y  = 14.0;      // against the +Y wall, far corner from the OLED
mic_pad_d  = 4.0;
mic_stand  = 3.0;
mic_port_d = 3.0;       // acoustic port through the wall
mic_port_z = 8.0;

/* ---------- lid fasteners: 4 x M3 self-tapping ---------- */
post_d    = 6.4;
scr_pilot = 2.5;
scr_shaft = 3.2;
scr_head  = 6.0;
post_in   = 5.2;        // post centre, inset from the inner cavity corner

ext_x = inner_x + 2 * wall;
ext_y = inner_y + 2 * wall;

module rrect(x, y, r, h) {
    hull() for (sx = [-1, 1], sy = [-1, 1])
        translate([sx * (x / 2 - r), sy * (y / 2 - r), 0])
            cylinder(r = r, h = h);
}

function post_xy(i) = [
    (i == 0 || i == 3 ?  1 : -1) * (inner_x / 2 - post_in),
    (i < 2            ?  1 : -1) * (inner_y / 2 - post_in)
];

/* =======================  BASE  ======================= */
module base() {
    difference() {
        union() {
            rrect(ext_x, ext_y, fillet, floor_t + inner_z);

            // corner posts for the lid screws
            for (i = [0 : 3]) {
                p = post_xy(i);
                translate([p[0], p[1], floor_t - 0.01])
                    cylinder(d = post_d, h = inner_z);
            }

            // pads the devkit rests on
            for (sx = [-1, 1], sy = [-1, 1])
                translate([sx * (dk_x / 2 - 4), dk_off_y + sy * (dk_y / 2 - 4), floor_t - 0.01])
                    cylinder(d = pad_d, h = dk_standoff);

            // ribs that locate the devkit laterally
            rib_z = dk_standoff + dk_t + rib_h;
            for (sy = [-1, 1])
                translate([0, dk_off_y + sy * (dk_y / 2 + clr + 0.6), floor_t])
                    linear_extrude(rib_z)
                        square([dk_x * 0.55, 1.2], center = true);
            for (sx = [-1, 1])
                translate([sx * (dk_x / 2 + clr + 0.6), dk_off_y, floor_t])
                    linear_extrude(rib_z)
                        square([1.2, dk_y * 0.5], center = true);

            // microphone pads
            for (sx = [-1, 1], sy = [-1, 1])
                translate([mic_off_x + sx * (mic_x / 2 - 2.5),
                           mic_off_y + sy * (mic_y / 2 - 2.5), floor_t - 0.01])
                    cylinder(d = mic_pad_d, h = mic_stand);
        }

        // cavity
        translate([0, 0, floor_t])
            rrect(inner_x, inner_y, max(0.6, fillet - wall), inner_z + 1);

        // screw pilots
        for (i = [0 : 3]) {
            p = post_xy(i);
            translate([p[0], p[1], floor_t + inner_z - 11])
                cylinder(d = scr_pilot, h = 12);
        }

        // USB-C openings
        for (s = [-1, 1])
            translate([-inner_x / 2 - wall / 2,
                       dk_off_y + s * usb_gap / 2,
                       floor_t + dk_standoff + dk_t + usb_h / 2 - 0.6])
                cube([wall * 4, usb_w, usb_h], center = true);

        // acoustic port
        translate([mic_off_x, inner_y / 2 + wall / 2, floor_t + mic_port_z])
            rotate([90, 0, 0])
                cylinder(d = mic_port_d, h = wall * 4, center = true);
    }
}

/* =======================  LID  ======================= */
module lid() {
    difference() {
        union() {
            rrect(ext_x, ext_y, fillet, lid_t);

            // lip ring that drops into the cavity and locates the lid
            translate([0, 0, -1.8])
                difference() {
                    rrect(inner_x - 2 * clr, inner_y - 2 * clr, max(0.6, fillet - wall), 1.8);
                    translate([0, 0, -0.5])
                        rrect(inner_x - 2 * clr - 3.0, inner_y - 2 * clr - 3.0,
                              max(0.4, fillet - wall - 1.5), 3.0);
                }

            // OLED mounting bosses
            for (sx = [-1, 1], sy = [-1, 1])
                translate([ol_off_x + sx * ol_hole_dx / 2,
                           ol_off_y + sy * ol_hole_dy / 2, -ol_boss_h])
                    cylinder(d = ol_boss_d, h = ol_boss_h + 0.01);
        }

        // display window
        translate([ol_off_x, ol_off_y, -ol_boss_h - 2])
            linear_extrude(lid_t + ol_boss_h + 4)
                square([ol_win_x, ol_win_y], center = true);

        // OLED pilot holes
        for (sx = [-1, 1], sy = [-1, 1])
            translate([ol_off_x + sx * ol_hole_dx / 2,
                       ol_off_y + sy * ol_hole_dy / 2, -ol_boss_h - 0.5])
                cylinder(d = ol_hole_d, h = ol_boss_h + 1);

        // lid screws, countersunk from the top
        for (i = [0 : 3]) {
            p = post_xy(i);
            translate([p[0], p[1], -3]) cylinder(d = scr_shaft, h = lid_t + 6);
            translate([p[0], p[1], lid_t - 1.6])
                cylinder(d1 = scr_shaft, d2 = scr_head, h = 1.7);
        }
    }
}

/* =======================  LAYOUT  ======================= */
if (part == "base") base();
else if (part == "lid") lid();
else {
    base();
    translate([0, ext_y + 6, 0]) lid();
}
