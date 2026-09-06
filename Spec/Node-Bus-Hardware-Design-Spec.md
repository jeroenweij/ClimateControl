# Node Bus — Hardware Design Spec
### Power + connector decisions for the STM32G030F6P6TR RS485 node network

**Status:** Draft — decisions locked from design discussion, pending your final servo current numbers and cable-run distances (see §7)
**Companion doc:** `RS485-Node-Protocol-Spec-STM32G030.md` (wire protocol / framing / CRC — this doc is physical layer only)

---

## 1. Topology overview

- 1 master + up to 20 slave nodes on a single half-duplex RS485 bus.
- Physical medium: standard 8-conductor Ethernet patch cable, daisy-chained node-to-node (each node has an "in" and "out" RJ45).
- One cable carries **power, RS485 data, and two control lines** simultaneously — no separate power harness for the common case.
- RS485 data pair runs as one continuous bus end-to-end. **Power is injected at multiple points along the chain, not solely from the master end** — see §4.

---

## 2. Bus voltage: 48V (PoE-class)

**Decision: run the shared power rail at 48V DC**, stepped down locally at each node to 5V (servo) and 3.3V (MCU).

**Why 48V over 12V/24V:**
- Cable/connector current scales as `I = P / V`. At a fixed servo power draw, 48V draws **1/4 the current of 12V** and **1/2 the current of 24V** for the same delivered power — this is what makes the RJ45 connector current budget (§4) workable at all with 20 nodes on one chain.
- Directly comparable to established PoE practice (802.3af/at/bt operate in this voltage class for exactly this reason).

**Consequence — hot-plug inrush:** higher rail voltage means more energy dumped into each node's input bulk capacitor on connection. **Each node needs inrush limiting** (NTC thermistor or soft-start MOSFET) on its 48V input to avoid connector-pitting sparks on hot-plug.

**Consequence — regulator selection:** the 5V rail needs a wide-input **synchronous buck** (not LDO — at 48V→5V an LDO would dissipate ~90% of input power as heat). The 3.3V rail can be a simple buck or LDO fed from the already-regulated 5V rail.

---

## 3. Frame/pin assignment (per RJ45, T568 pairing)

| Pins | Pair (color) | Assignment | Notes |
|---|---|---|---|
| 4, 5 | Blue (center pair) | **RS485 A/B** (differential) | Center pair chosen for tightest coupling of the four pairs and maximum physical separation from both power pairs |
| 1, 2 | Orange | **V+ (48V, bonded)** | Both conductors bonded in parallel — halves effective resistance per rail vs. a single conductor |
| 7, 8 | Brown | **GND return (bonded)** | Symmetric with V+ pair, opposite outer edge of the connector |
| 3, 6 | Green | **ENABLE** (bonded, both wires) | Decided 2026-09-06: dropped the separate RESET/`NRST` bus — one control line is enough (see below). Both green conductors now bonded together for ENABLE, same halved-resistance rationale as the V+/GND pairs, rather than leaving the second wire unused. |

**ENABLE line:** **push-pull, master-driven only**, single driver, no contention — feeds each node's power-switch/regulator-enable input (the buck converter's EN pin, see `Node-Bus-Power-Path-Spec.md` §5). 

**Drive voltage: 48V (bus line voltage), not a logic level.** Decided 2026-09-06, superseding an earlier 5V-drive plan — see the margin analysis below for why. Driven from the master via a **low-side N-MOSFET pulling the line to GND (disable) with a pull-up resistor to 48V (enable)**, rather than a true push-pull stage — simpler, and sufficient since the master is the only driver:

| Component | Value | LCSC # | Notes |
|---|---|---|---|
| Switch | onsemi BSS123, N-channel MOSFET | **C513249** | SOT-23, 100V Vds (margin over the 48V line), Vgs(th) 1.7V (turns on solidly off a 3.3V master GPIO), 170mA continuous (well above the ~1mA this needs) |
| Gate series resistor | 330Ω | commodity | |
| Gate pull-down resistor | 10kΩ | commodity | Holds the MOSFET off (line pulled high/enabled... or disabled, depending on chosen polarity) if the master's GPIO is floating/tri-stated during boot, so the bus doesn't get disabled by accident |
| Line pull-up resistor (ENABLE net to 48V) | 47kΩ | commodity | Sized for low power dissipation (~1mA, ~50mW) — this is a slow, non-timing-critical control line, not a data line |

This is a fresh design for this specific need, not reused from anywhere — `~/git/node/Hardware/node.sch`'s MOSFET_1/MOSFET_2 output channels (Q2/Q3) looked like a similar topology at a glance, but those are unrelated, generic user-configurable device-driver outputs, populated per-deployment only when actually used — not a bus-control precedent.

The LMR16030's EN pin tolerates 0–60V with only a ~1.2V threshold (datasheet §6.1/§6.5), so 48V is well within its rated range — no extra protection needed at the receiving end.

**Why no separate RESET line:** power-cycling a node via ENABLE also resets its MCU as a side effect, making ENABLE a strict superset of what a dedicated `NRST` bus would do — the only capability lost is resetting the MCU *without* dropping the whole node's power, which isn't needed here (not battery-powered, and the fuse/inrush design already tolerates power-cycling transients). If a future need for a glitch-free soft-reset-only path emerges, revisit.

**Voltage-drop / ground-offset risk — resolved by the 48V drive choice.** Since ENABLE is single-ended (referenced to each node's local GND), the exposure is **ground offset** between the master and a far node, caused by the shared bonded GND-return pair (pins 7/8) carrying the cumulative return current of every downstream node. With the bus length now known (~100m total, see §7 item 3), a segment-by-segment estimate (24AWG bonded pairs, ~4.69Ω/100m per path, 20 nodes evenly spaced) puts that ground offset at roughly **3.45V under normal cumulative load (~1.4A) and ~11.6V under worst-case simultaneous stall (~4.7A)** — numbers that would have eaten most or all of a 5V-drive line's margin (~3.6V above the 1.38V worst-case threshold), which is exactly why the drive voltage moved to 48V instead: at 48V, the same worst-case offset still leaves **~36V of margin**, making this a non-issue rather than something to manage. A Schmitt-trigger + small RC filter at each node's receiving end is still cheap insurance against fast transient noise (from the local buck's own 500kHz switching, or neighboring nodes' converters), but is no longer load-bearing for the DC margin question the way it would have been at 5V.

**The ENABLE line is hard-wired straight through each node's in/out RJ45** (not buffered or re-driven by the node MCU), so a hard kill works even if a node's firmware is hung.

**Note on pair-selection rationale:** at RS485 baud rates, inter-pair crosstalk is not the primary concern this layout solves — that matters far more at gigabit-Ethernet speeds. The real reason to keep the RS485 pair away from the power pairs is broadband switching noise from each node's buck regulator; good decoupling at each node's power input matters more here than which physical pair carries which signal.

---

## 4. Connector current budget — the constraint that shapes the power topology

**Problem:** in a pure daisy chain, the cable segment nearest the master carries the *cumulative* current of all downstream nodes, not just the local node's draw. RJ45 contacts on generic shielded jacks (see §5) are rated **1.5A per contact**; bonding two contacts per rail (as in §3) gives roughly **2–3A of safe headroom per hop** at the head end.

**At 48V, converted from 5V-side servo current** (`I_bus = P_load / (V_bus × η)`, η ≈ 0.85–0.90 for a synchronous buck):

| Scenario | 5V-side total (20 nodes) | Actual 48V bus current |
|---|---|---|
| All servos running normally | ~12 A | **~1.4 A** |
| All 20 servos stalled simultaneously (worst case) | ~40 A | **~4.7 A** |

**Decision:**
- Normal operation (~1.4A) is comfortably within a bonded-pair connector's safe range — no special handling needed for the typical case.
- Worst-case simultaneous full-stall (~4.7A) can exceed the ~2–3A single-hop connector budget.
- **Mitigation — committed 2026-09-06: power injection at both ends of the chain**, not multi-point injection every 4–5 nodes as originally sketched. With the bus length now known (~100m, §7 item 3), feeding both ends splits each node's return current toward whichever end is electrically closer, cutting the worst-case IR drop to roughly 1/4 of the single-end-fed case (standard result for a distributed load fed from both ends) — simpler than 4-5 separate injection points along the run, and sufficient: see §6 for the RS485 common-mode margin this resolves. Requires an actual physical power connection at the far end of the run (a second supply there, or a dedicated feed cable back to the master), not just something the daisy-chain connectors handle on their own. RS485 stays a single continuous bus end-to-end regardless — only the power pairs need the second feed point.
- **Regardless of the above, every node gets a fuse or PTC on its 48V input** — cheap insurance against a single node fault pulling down the shared rail, and it caps that node's worst-case contribution to bus current.

---

## 5. RJ45 connector part

**Selected (2026-09-06, superseding the original pick below): Hong Cheng HC-WK88-H18-B — LCSC/JLCPCB part C7501838**

Switched from RCH RC00156 (C708597) because that part had limited stock at order time. Verified against its datasheet (not just the listing attributes) before switching:

| Spec | Value |
|---|---|
| Type | 8P8C (RJ45), single port |
| Magnetics | **None** — confirmed from the datasheet pinout drawing (plain 8-pin pass-through, no transformer/coil circuitry, no "integrated magnetics" callout) — same critical requirement as before: a magnetics/transformer jack would block DC power and distort RS485 |
| Shielding | Yes, metal shell (stainless steel) |
| LED | None |
| Mount | Through-hole, right-angle |
| Current rating | 1.5A per contact — matches the connector current budget this doc's §4 calculations are already based on |
| Voltage rating | 125 VAC RMS (comfortably above the 48V DC bus) |
| Stock (at last check) | 2,535 units |

Two of this same part are used per node (one "in," one "out"), per §1 — using one part number for both keeps the footprint/BOM simple.

**Runner-up considered, not chosen:** C7501830 (Hong Cheng HC-WK88-H132-55B) — same electrical spec (1.5A/contact, 125VAC, shielded, through-hole, no magnetics confirmed the same way) and slightly higher stock (3,030), but a different housing variant in the same series — not footprint-interchangeable with C7501838. Reasonable fallback if C7501838 stock runs out.

**Note:** this part is from a different manufacturer/series than the original RCH RC00156 pick, so its footprint is not a drop-in match for anything already laid out against that part — verify the footprint against the board before PCB layout.

**Before BOM lock:** verify pin numbering and shield-pin location against the schematic (already checked against the datasheet above, but re-verify once placed); tie the shield to chassis/earth ground rather than signal ground if a separate protective earth is available.

<details>
<summary>Original pick (superseded above)</summary>

**RCH RC00156 — JLCPCB part C708597**

| Spec | Value |
|---|---|
| Type | 8P8C (RJ45), single port |
| Magnetics | None — plain pass-through contacts |
| Shielding | Yes, metal shell |
| LED | None |
| Mount | Through-hole |
| Current rating | 1.5A per contact |
| Stock | Ran low at order time — this is why the part was switched, see above |

Alternative that was on the shortlist alongside it: RCH RC01186 (JLCPCB C708619) — 2-port stacked version, no magnetics, no LED, would let one part serve both the "in" and "out" jack per node. Never adopted.

</details>

---

## 6. USART / driver-enable (STM32G030F6P6TR)

- **Transceiver: Maxim/ADI MAX3485CSA** — LCSC `C2687394`, SO-8. Recorded 2026-09-06, found in `~/git/node/Hardware/Production/bom.csv` (U1) — the actual part used in the prior-generation node's real production build, not just its schematic. Resolves a gap no spec had answered until now (the protocol spec's §2 table only said "existing RS485 transceiver (reuse)" without naming one).
- Use **USART1's hardware Driver-Enable (DE) output** to control the transceiver's DE/RE pins, instead of a manually toggled GPIO with software delays (as the old ATmega-based design required).
- This removes the `setEnable()`-style delay loop entirely — the peripheral handles assertion/de-assertion timing (`DEAT`/`DEDT`) automatically per transmission.

**Common-mode range check against the ground-offset numbers in §3:** MAX3485 has the standard EIA-485 receiver common-mode input range, **-7V to +12V** (absolute max -7.5V to +12.5V). Checked against this doc's ground-offset estimates (§3, same shared-GND-return mechanism, same numbers apply here — RS485 is differential so it doesn't share ENABLE's exact failure mode, but it does share the same ground-reference-offset exposure):
- Original single-end-fed estimate: ~3.45V (normal) / ~11.6V (worst-case stall) — the stall figure sat right at the edge of the +12V limit, not comfortably resolved.
- **Resolved 2026-09-06 by the both-ends power injection decision in §4:** feeding both ends of the chain cuts worst-case IR drop to roughly 1/4, revising these to **~0.86V (normal) / ~2.9V (worst-case stall)** — now comfortably inside the ±12V window with better than 4x margin even in the worst case. Unlike ENABLE, RS485's common-mode range is fixed by the transceiver silicon (no "just drive it higher" option available), so reducing the underlying ground offset via §4's fix was the only real lever — and it works.
- §7 items 1–2 (real stall current, and whether simultaneous full-stall is realistic) still matter for sizing the fuse/PTC and connector budget in §4, but are no longer signal-integrity-critical for RS485 the way they were before the both-ends decision.

---

## 7. Open items — need your input before finalizing

1. **Real servo stall current** — §4's numbers use a generic placeholder. With both-ends power injection now committed (§4), this mainly affects fuse/PTC sizing and connector budget rather than signal integrity (§6's RS485 margin concern is resolved regardless, with >4x headroom even at the current placeholder stall estimate).
2. **Whether simultaneous full-stall across all 20 nodes is a realistic scenario** for your application (e.g. synchronized power-on homing) or a non-issue because servos move independently — no longer signal-integrity-critical now that both-ends injection (§4) resolved the RS485 margin question (§6) with comfortable headroom either way. Still relevant for fuse/PTC and connector-current sizing.
3. ~~**Cable run length per segment**~~ — partially resolved 2026-09-06: total bus length ~100m (used in §3's ENABLE ground-offset estimate, and the 48V drive decision there absorbed the risk regardless). Per-segment breakdown and RS485 baud-rate implications not yet revisited against this number.
4. ~~**Final connector choice**~~ — resolved 2026-09-06: C7501838, two single-port jacks per node (see §5). Footprint still needs verification against the board layout once you're at PCB stage.
5. ~~**ENABLE line noise filtering**~~ — downgraded 2026-09-06: no longer load-bearing now that ENABLE drives at 48V (see §3) rather than 5V — the Schmitt-trigger + RC filter is optional insurance against transient noise, not a fix for a margin problem that no longer exists at this drive voltage. Add it if cheap, skip it if not without much consequence either way.
