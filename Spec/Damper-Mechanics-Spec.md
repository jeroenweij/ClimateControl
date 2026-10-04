# Damper Mechanics — Design Spec
### The installed duct damper (Lindab DRU) and how the ControllerNode servo mounts on and drives it

**Companion docs:** `Node-Bus-Hardware-Design-Spec.md` §4 (DS3225 servo, linkage summary), `Node-Bus-Power-Path-Spec.md` §3.1 (servo rail gating, stall detection), `ControllerNode-Bringup-Test-Plan.md` §6 (linkage and end-point checks), `Damper-Budget-Spec.md` (what drives the target position).

Design files in `Hardware/Damper/`: printable parts as STL in `3dFiles/` (modelled in Tinkercad), assembly renders in `Assembly images/`, site photos of an installed damper in `build/`.

---

## 1. The damper: Lindab DRU

The room branches use **Lindab DRU** regulating dampers: a galvanised steel push-in fitting with a single turning, cut-off blade. The identification comes from the site photos and matches Lindab's description point for point:

- The blade is set with a **flat wing knob inside a protective steel cup**. The cup stands out from the duct so that about 50 mm of insulation leaves the knob visible, which is how it is installed here.
- The blade angle is read against an **embossed scale on the rim of the cup**.
- The knob is locked by **two Pozidriv screws (PZD2)** running in arc slots in the knob disc.

Published data (Lindab DRU datasheet, DUCT-MC 17.11.002, and the Lindab dampers mounting instructions):

| Property | Value |
|---|---|
| Nominal duct diameter Ød1 | Ø80–630 with a knob in a cup; residential branches are typically Ø100 / 125 / 160 |
| Fitting length l | 100 mm (Ø80–450) |
| Cup height above the duct surface | **45 mm** |
| Insulation allowed without hiding the knob | about 50 mm (the optional IK cup extends this to 100 mm) |
| Blade travel | stepless **0–90°**, where **α = 0° is fully open** and **α = 90° is closed** |
| Sealing past the closed blade | class 0. The blade is cut off at two sides, so it never seals completely |
| Lindab's own motorisation (small sizes) | Belimo CM24 / CM24-SR, **2 Nm** minimum. This is the torque Lindab considers sufficient for the blade |
| Motor-ready variant | available to order. Its shaft end carries a notch that shows the blade position |

Not published, measured on site: the cup is a Ø70 mm steel tube whose top edge is rolled out into a flange of up to about Ø88 mm, and that flange has a **cutout** at one point of its circumference. The cutout is a fixed reference on the damper body. Seen from above with the cutout at the bottom, the handle **vertical** (pointing at the cutout) is **closed** and the handle **horizontal** is **open**.

## 2. Mounting: a ring keyed onto the cup's rim

The actuator mounts on the **damper's own cup**, not on the duct or the building structure. The cup is rigid steel and belongs to the damper body, so the servo and the knob stay aligned however the insulated duct flexes or settles, and nothing bears on the insulation.

Printed parts (`Hardware/Damper/3dFiles/`):

| Part | File | Role |
|---|---|---|
| Main clamp | `DamperDrive_MainClamp_and_servo_mount.stl` | One half of the ring, with the plate that carries the servo and the ControllerNode PCB |
| Clamp | `DamperDrive_Clamp.stl` | The other half of the ring |
| Bearing retainer | `DamperDrive_Bearing_retainer.stl` | Holds the bearing's outer ring down onto the ring and ties the two halves together |
| Fork | `DamperDrive_Pinoin.stl` | Shaft on the damper axis: fork on the knob below, bearing seat in the middle, key block for the driven gear on top |
| Driven gear | `DamperDrive_BigGear.stl` | Toothed sector on the fork |
| Pinion | `DamperDrive_SmallGear.stl` | On the servo horn |

**The ring.** Two halves close around the cup: outside Ø100 mm, 30 mm tall, Ø70 mm bore. Towards the top the bore opens into a conical groove, ending in a short Ø90 mm band, that takes the cup's rolled flange, and above it a lip (inside Ø59 mm) closes over the flange. The ring is therefore located on the flange rather than clamped by friction on the tube. The halves are bolted to each other across both split faces (two horizontal screws per face), and the bearing retainer's six M3 screws tie them together from above.

**Anti-rotation key.** A half-round **nub** (about 7 mm wide) under the lip of the main clamp engages the flange **cutout**, like a key in a keyway. It takes the full reaction torque of the drive, about 120 N at its radius of 42.5 mm at servo stall (§3.2). It also fixes the whole actuator's orientation on the damper, and with it the relation between the servo, the gear sector and the blade scale (§3.1). The nub should fit the cutout closely, because any play there adds directly to the gear backlash when the drive reverses.

Before the ring goes on, a collar of insulation around the cup is cut away. The foil is re-taped up to the ring afterwards. Once the ring is unbolted it comes off as two halves, and the knob is free to set by hand again (manual fallback).

**Bearing.** The fork turns in a **6908ZZ** deep-groove ball bearing (40 × 62 × 12 mm, shields on both sides):

- The outer ring sits in the retainer's Ø62.2 bore. It rests on the ring's top lip and is held down by the retainer's top lip. Both lips bear only on the outer ring's face and stay clear of the shields.
- The inner ring is clamped between the fork's Ø43 flange below and the driven gear's Ø44.5 hub above.

The ring and bearing take the gear-mesh side load, so the knob only sees torque.

## 3. Drive: a fork on the knob, geared 2.1:1

- The **fork** reaches down into the cup and straddles the knob's wing bar with a 10 mm wide slot. The wing bar is the only drive interface, and the damper itself is not modified. The fork is short enough that its underside stays clear of the PZD2 screw heads, which stay put while the knob turns under them.
- The **driven gear** sits on the fork's 20 × 20 mm key block and is bolted to it with eight M3 screws. It runs above the ring, on the damper's axis.
- The **pinion** is bolted to the DS3225's metal single-arm 25 T horn: the arm sits in a 6 mm recess, one M3 screw goes through the arm, and a Ø6 hole gives access to the horn screw. The pinion does not print the spline, which would wear out.
- The **servo** drops through a 20.5 × 41.5 mm pocket in the plate. It is held on its rubber grommets with the metal inserts and screws supplied with it, on a 48 × 10 mm hole pattern that matches the servos as bought. The gear-mesh force pushes the servo sideways across the pocket, so the pocket walls (0.25 mm clearance) carry that load, not the rubber.
- The **PZD2 lock screws are loosened** so that the knob turns. Left just snug, they add a little friction that helps the unpowered servo hold the blade against airflow.

### Gear set

| | Pinion (servo) | Driven gear (fork) |
|---|---|---|
| Teeth | 20 | 42-tooth pitch, cut as a sector |
| Tip diameter | 68.6 mm | 137.3 mm |
| Module | ≈ 3.1 | ≈ 3.1 |
| Face width | 12 mm | 12 mm |
| Centre distance | 98.5 mm | |
| Backlash | ≈ 0.5° of blade (≈ 0.55 mm at the mesh), constant over the travel | |

The driven gear only needs teeth where the pinion runs. The toothed sector spans about ±64° around the mesh line, against ±43° of travel, which leaves about 21° of spare teeth at each end. The rest of the gear is a solid plate (Ø90 at the back) that also covers the cup.

The gear size comes from two limits rather than the ratio:

- **Centre distance.** The servo stands beside the cup, with the pinion in the plane of the driven gear above the ring. The centre distance therefore has to clear the ring (Ø100) and the servo body. 98.5 mm does that.
- **Tooth strength at stall.** The Lewis bending stress at the pinion root, at the DS3225's 2.45 Nm stall torque, is about 6–7 MPa for this module and face width. Printed PETG or ASA holds about 30–40 MPa. For comparison, module 1.5 would sit at about 34 MPa and module 1 would fail.

The backlash above was checked by sweeping the meshing tooth profiles through the full travel. It is less than one 1 % step. Printing usually takes a little of it away rather than adding to it.

### 3.1 Travel and end points

| Servo | Blade |
|---|---|
| 0–180° (0–100 %, 500–2500 µs) | **≈ 86°** (180° / 2.1) |
| 1 % step | ≈ 0.86° |

The nub fixes the housing to the flange cutout, and the sector is centred on the mesh with the knob at half-open: the wing bar at 45° to the cutout direction, midway between vertical (closed) and horizontal (open). With the pinion set to the servo's 50 % position there, full travel covers about 2° to 88° of the damper's 0–90°:

- **0 % (`DamperTarget` 0):** blade about 2° short of closed. It still leaves the minimum ventilation gap and stays clear of the arc-slot end.
- **100 %:** blade about 2° short of fully open, also clear of the slot end.

The servo therefore never drives into a hard stop, and firmware has no per-unit trim or software end stops (`Modules/ControllerNode/Damper.h`). A stall anywhere in the range is a mechanical fault. The damper's own convention runs the other way from the firmware's (DRU α = 0° is open, `DamperTarget` 0 is closed). The housing can only go on one way, so the direction is fixed by the design. `ControllerNode-Bringup-Test-Plan.md` step 6.3 checks it.

### 3.2 Torque

| | Torque at the knob |
|---|---|
| Lindab's own actuator for this damper | 2 Nm |
| DS3225 stall (25 kg·cm ≈ 2.45 Nm) × 2.1 | **≈ 5.1 Nm** |

The reduction gives more than twice the torque the damper needs. It also means that a knob run into its slot end is loaded to about 5.1 Nm until stall detection cuts the rail (`stallConfirmMs`, about 200 ms). The same torque comes back through the ring into the flange cutout (§2). The printed fork and gear are sized to be the weakest link, so they fail before the damper's knob.

## 4. Assembly on site

1. Loosen the two PZD2 screws and check that the knob turns freely across its range.
2. Cut the insulation back around the cup.
3. Fit the two ring halves around the flange with the main clamp's nub in the cutout. Bolt the halves together across the split faces.
4. Put the 6908ZZ bearing on the fork and drop the fork into the ring, engaging the knob's wing bar. Screw the bearing retainer down onto the ring.
5. Set the knob to half-open (wing bar at 45° to the cutout direction). Bolt the driven gear onto the fork's key block.
6. Command `DamperTarget` 50 with the servo in its pocket and the horn fitted. Fit the pinion onto the horn so that it meshes in the middle of the sector, and screw it down.
7. Snug the PZD2 screws lightly as a friction brake, not as a lock.
8. Run `ControllerNode-Bringup-Test-Plan.md` §6.

## 5. Open items

1. **Bearing fit:** the 6908ZZ pockets are drawn at the bearing's nominal sizes (12.00 mm deep, Ø40 shaft, Ø62.2 bore), with no axial play allowed for. Check the fit on the printed parts with real bearings and adjust the pocket depth or bores if it binds or rattles.
2. **Printed material:** PETG or ASA, for duct temperatures and for long-term creep under the bolt and key loads. Not yet tried on a real damper.
