# Damper Mechanics — Design Spec
### The installed duct damper (Lindab DRU) and how the ControllerNode servo mounts on and drives it

**Companion docs:** `Node-Bus-Hardware-Design-Spec.md` §4 (DS3225 servo, linkage summary), `Node-Bus-Power-Path-Spec.md` §3.1 (servo rail gating, stall detection), `ControllerNode-Bringup-Test-Plan.md` §6 (linkage and end-point checks), `Damper-Budget-Spec.md` (what drives the target position).

Site photos of an installed damper: `Hardware/Damper/`.

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

Not published, and still to be measured on site (§5): the cup's outer diameter and wall thickness, the knob's disc diameter and wing-bar dimensions, and the arc-slot angle.

## 2. Mounting: a ring clamped around the cup

The actuator mounts on the **damper's own cup**, not on the duct or the building structure:

- A **3D-printed ring** fits around the outside of the steel cup and is clamped to it, for example with a band or hose clip around the ring. The servo bracket is part of this ring.
- The cup is rigid steel and belongs to the damper body, so the servo and the knob stay aligned however the insulated duct flexes or settles. Nothing bears on the insulation.
- To give the ring a clamping surface on the cup's outer wall, a collar of insulation around the cup is cut away. The foil is re-taped up to the ring afterwards.
- The ring slides off once the clamp is released. The knob is then free to set by hand again (manual fallback).

## 3. Drive: a fork on the knob, geared 2.2:1

- A **fork** reaches down into the cup and straddles the knob's wing bar. The wing bar is the only drive interface, and the damper itself is not modified.
- The fork carries the **driven gear** above the cup rim, on the damper's axis. It turns in a bearing in the ring, so the ring takes the gear-mesh side load and the knob only sees torque.
- A **pinion on the servo horn** meshes with the driven gear at a **2.2:1 reduction**.
- The **PZD2 lock screws are loosened** so that the knob turns. Left just snug, they add a little friction that helps the unpowered servo hold the blade against airflow.

### Gear set

**20 T pinion : 44 T driven gear, module 2** (20° pressure angle, 3D-printed in PETG or ASA):

| | Pinion (servo) | Driven gear (fork) |
|---|---|---|
| Teeth | 20 | 44 |
| Pitch diameter | 40 mm | 88 mm |
| Outside diameter | 44 mm | 92 mm |
| Centre distance | 64 mm (print +0.2 mm for backlash) | |
| Face width | 10–12 mm | |

The gear size is set by two limits, not by the ratio:

- **Centre distance.** The servo stands beside the cup, with the pinion in the plane of the driven gear above the rim. The centre distance must therefore clear the cup radius (about 35–40 mm, estimated), the ring wall (about 4 mm) and about 10 mm from the servo's output shaft to its body edge. That puts the minimum at about 55–60 mm, which 64 mm clears with room to spare.
- **Tooth strength at stall.** Lewis bending stress at the pinion root, 10 mm face width, at the ≈ 5.4 Nm stall torque (§3.2):

  | Module (20 : 44) | Centre distance | Root stress at 2 Nm | Root stress at 5.4 Nm stall |
  |---|---|---|---|
  | 1 | 32 mm | 28 MPa | 76 MPa, fails |
  | 1.5 | 48 mm | 13 MPa | 34 MPa, marginal |
  | **2** | **64 mm** | **7 MPa** | **19 MPa** |

  Printed PETG or ASA holds about 30–40 MPa, so module 2 is the smallest size that survives a stall with margin. A larger module is stronger again and only moves the servo further out.

Other properties of this gear set:

- The driven gear's 92 mm outside diameter is larger than the cup, so it also works as a dust cover over the cup.
- 20 teeth avoids undercut on the pinion.
- 0.2–0.3 mm of printed backlash at the driven gear's 44 mm pitch radius is about 0.3° of blade, less than one 1 % step.
- The pinion screws onto the DS3225's metal 25 T spline horn instead of printing the spline, which would wear out.

Any gear set with a 2.2:1 ratio keeps the travel in §3.1. Printed gears need not use a standard module: for 20 : 44, module = centre distance / 32.

### 3.1 Travel and end points

| Servo | Blade |
|---|---|
| 0–180° (0–100 %, 500–2500 µs) | **≈ 82°** (180° / 2.2) |
| 1 % step | ≈ 0.82° |

The damper's 0–90° range leaves about 8° of margin, which is split between the two ends when the fork is set on the knob:

- **0 % (`DamperTarget` 0):** blade short of closed. It still leaves the minimum ventilation gap and stays clear of the arc-slot end.
- **100 %:** blade short of fully open, also clear of the slot end.

The servo therefore never drives into a hard stop, and firmware has no per-unit trim or software end stops (`Modules/ControllerNode/Damper.h`). A stall anywhere in the range is a mechanical fault. The damper's own convention runs the other way from the firmware's (DRU α = 0° is open, `DamperTarget` 0 is closed). The fork is set so that 0 % closes, as checked by `ControllerNode-Bringup-Test-Plan.md` step 6.3.

### 3.2 Torque

| | Torque at the knob |
|---|---|
| Lindab's own actuator for this damper | 2 Nm |
| DS3225 stall (25 kg·cm ≈ 2.45 Nm) × 2.2 | **≈ 5.4 Nm** |

The reduction gives more than twice the torque the damper needs. It also means a misadjusted fork, or a knob run into its slot end, is loaded to about 5.4 Nm until stall detection cuts the rail (`stallConfirmMs`, about 200 ms). The printed fork and gear are sized to be the weakest link, so they fail before the damper's knob.

## 4. Assembly on site

1. Loosen the two PZD2 screws. Check that the knob turns freely across its range and note on the cup scale which way is open.
2. Cut the insulation back around the cup and fit the ring. Do not tighten the clamp yet.
3. Command `DamperTarget` 50. With the servo powered, set the knob to about half open (α ≈ 45° on the cup scale). Seat the fork on the wing bar and engage the gears.
4. Tighten the ring clamp. Snug the PZD2 screws lightly as a friction brake, not as a lock.
5. Run `ControllerNode-Bringup-Test-Plan.md` §6.

## 5. Open items

1. **Site measurements:** cup outer diameter, wall thickness and height above the insulation; knob disc diameter; wing-bar width, thickness and height; arc-slot angle. Also the duct diameter of each branch (which DRU size), and the torque to turn the knob with the screws loosened.
2. **Final gear geometry:** a larger module than 2 is intended for extra tooth strength in the printed parts. It is to be generated with a gear generator (tool not yet chosen) and checked against the servo's clearance from the cup once the cup diameter is measured.
3. **Ring clamp detail:** band or hose clip around the printed ring, or grub screws. Also how much insulation has to be cut back.
4. **Printed material:** PETG or ASA for duct temperatures and long-term creep under the clamp load.
