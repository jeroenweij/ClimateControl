# PCB Layout Rules — ClimateControl node boards

Short checklist for laying out the Base and Main boards. Both share the same
stackup and the same rules; the buck converter drives most of them.

---

## Stackup

**4 layers:  L1 sig · L2 GND (solid) · L3 power · L4 sig**

- L2 is one **unbroken ground plane** — no signals, no splits, no keepouts through it.
- L3: 5 V and 3.3 V copper pours; low-priority routing allowed.
- 1 oz copper. Use **2 oz outer** on the Main board if the 48 V area is tight.
- JLCPCB standard 4-layer stackup, no impedance control needed.

## EasyEDA design rules (fab minimums — JLCPCB 4-layer)

| Rule | Value |
|---|---|
| Trace / space (default) | 0.15 mm / 0.15 mm |
| Trace / space (min, only if a part forces it) | 0.09 mm / 0.09 mm |
| Via | 0.3 mm hole / 0.6 mm pad |
| Via-in-pad | avoid; if unavoidable, tented |
| Annular ring | ≥ 0.13 mm |
| Silk / edge clearance | 0.2 mm |
| Board edge to copper | 0.3 mm |

## Current-carrying traces (1 oz, ~10 °C rise)

| Net | Width |
|---|---|
| 48 V bus (RJ45 in↔out pass-through, P1 feed) | **2 mm or a pour** |
| 5 V servo feed | **1 mm** |
| 3.3 V, ENABLE, signals | 0.25 mm is fine |

---

## The buck (LMR16030) — do this first

1. **Input loop tiny:** C1/C2 ceramics right on VIN↔PGND, shortest possible loop. This loop matters more than anything else on the board.
2. **Catch diode D1** in that same tight loop — SW and GND, close.
3. **SW node copper as small as possible** — just wide enough for current. It is a noise source; nothing sensitive near or under it.
4. **Bootstrap C6** right at the BOOT/SW pins.
5. **Inductor L1** next to SW. No traces routed underneath it.
6. **Output caps** right at L1's output pin.
7. **FB divider R1/R2** at the FB pin; route FB as a short quiet trace away from SW/L1, tap the output near the caps.
8. **RT resistor R3** at the RT pin.
9. **Exposed pad:** 3×3 via array to L2 and to a bottom-side GND pour (heat + return).
10. Solid L2 under the whole block; ring of stitching vias around it.

Keep the buck in its own corner, away from the MCU, the RS-485 transceiver, and any analog.

---

## Everything else

- **Decoupling:** 100 nF at *every* IC VCC pin, same side as the IC, its own via straight to L2. MCU pin 4 also gets the 1 µF + bulk close.
- **MCU:** NRST cap at the pin. SWD traces short. Away from the SW node.
- **RS-485 / RJ45:**
  - SM712 (U3) **right at the RJ45 pins**, A/B stubs as short as possible.
  - Transceiver (U7) close to the RJ45; route A/B as a pair over solid L2.
  - 120 Ω / bias footprints DNP but place them.
- **Protection TVS (SMAJ58A, SMCJ58A, SM712, ESD arrays):** place at the connector, *before* what they protect; shortest fat path to GND with its own via(s) to L2.
- **Main board 48 V input:** P1 → fuse holder → TVS → Schottky-or-link → +48 V rail, in that order, short and fat. Copper pour on any Schottky tab.
- **Electrolytics (C10, C11):** away from the buck and any hot part; watch height near the enclosure.
- **1-Wire (Main / TemperatureNode):** 4.7 kΩ pull-ups near the MCU; keep the ONEWIRE nets away from the SW node.
- **ENABLE MOSFET (Q1):** drain trace to the RJ45 ENABLE net; short.

## Mechanical / silk

- RJ45 shield → GND (or chassis pad if a separate earth exists).
- Silkscreen: polarity on P1, all diodes, electrolytics, LEDs; fuse value; connector pinouts; board name + revision + date.
- Mounting holes: pick GND-connected or isolated and be consistent.
- Fiducials if you're getting them assembled.

## Before ordering

- Run DRC. Run the LMR16030 loop check by eye (rule 1 above).
- Confirm the RJ45 and terminal-block footprints against their datasheets.
- Silkscreen readable, nothing under pads.
- Panelize Base + Main together, same fab order.
