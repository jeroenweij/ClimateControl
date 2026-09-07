# Node Bus — Hardware Design Spec
### Power, connector, and node-schematic decisions for the STM32G030F6P6 RS485 node network

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

**Consequence — hot-plug inrush:** higher rail voltage means more energy dumped into each node's input bulk capacitor on connection. Addressed by keeping that bulk cap small (47µF) rather than adding an NTC or soft-start MOSFET — see `Node-Bus-Power-Path-Spec.md` §2/§3.

**Consequence — regulator selection:** the 5V rail needs a wide-input **synchronous buck** (not LDO — at 48V→5V an LDO would dissipate ~90% of input power as heat). The 3.3V rail can be a simple buck or LDO fed from the already-regulated 5V rail.

---

## 3. Frame/pin assignment (per RJ45, T568 pairing)

| Pins | Pair (color) | Assignment | Notes |
|---|---|---|---|
| 4, 5 | Blue (center pair) | **RS485 A/B** (differential) | Center pair chosen for tightest coupling of the four pairs and maximum physical separation from both power pairs |
| 1, 2 | Orange | **V+ (48V, bonded)** | Both conductors bonded in parallel — halves effective resistance per rail vs. a single conductor |
| 7, 8 | Brown | **GND return (bonded)** | Symmetric with V+ pair, opposite outer edge of the connector |
| 3, 6 | Green | **ENABLE** (bonded, both wires) | Decided 2026-09-06: dropped the separate RESET/`NRST` bus — one control line is enough (see below). Both green conductors now bonded together for ENABLE, same halved-resistance rationale as the V+/GND pairs, rather than leaving the second wire unused. |

**ENABLE line:** master-driven only, single driver, no contention — feeds each node's buck-converter EN pin (see `Node-Bus-Power-Path-Spec.md` §5). **Enabled is the default state; the master only ever pulls the line low, to disable.**

**Drive scheme: one pull-up per node, one pull-down at the master.** Each node pulls the ENABLE net up to its *own local 48V rail* through a high-value resistor. The master has a single low-side N-MOSFET that pulls the whole net to GND to disable every node at once. There is no pull-up at the master.

Decided 2026-09-07, replacing the earlier single-master-pull-up scheme (2026-09-06). Putting the pull-up at each node *removes* the ground-offset problem instead of just cushioning it: a node now holds itself enabled from its own 48V and GND, so the shared line — and any IR drop or ground offset along it — no longer sets the "enabled" level. Only the disable edge travels the wire, carrying just the ~1mA total pull-up current, so drop along the ENABLE conductor is a few mV and irrelevant. This also retires the earlier "drive at 48V, not 5V" reasoning as a margin fix; the line still sits at 48V only because that is what the node pull-ups tie to.

| Component | Where | Value | Part | Notes |
|---|---|---|---|---|
| Pull-down MOSFET | master | onsemi BSS123 (N-ch) | LCSC C513249 | SOT-23, 100V Vds (margin over 48V), Vgs(th) 1.7V (full turn-on from a 3.3V GPIO), 170mA (vs. ~1mA needed) |
| Gate series resistor | master | 330Ω | commodity | |
| Gate pull-down resistor | master | 10kΩ | commodity | Holds the MOSFET off if the master GPIO floats at boot → line not pulled low → **bus stays enabled** |
| ENABLE pull-up | each node | **1.5MΩ** | FOJAN FRC1206F1504TS — LCSC C2933600 (1206 thick-film, 200V working voltage, 250mW, ±1%) | To the node's local 48V input. ~32µA/node; ~0.6mA / ~31mW across a full 20-node bus. Feeds the buck EN pin through a 10kΩ series + 10nF-to-GND filter (noise + EN abs-max protection). |

**Pull-up value — 1.5MΩ per node.** Across 20 nodes this parallels to ~75kΩ: ~0.6mA total draw, and a ~0.75ms enable-edge RC against ~10nF of bus capacitance — fine for a slow control line. Lower values just waste power; higher values get noise-sensitive for little gain. The full 48V sits across the pull-up whenever the line is held low, so the resistor's package voltage rating (not its value) is the real constraint — the selected 1206 part is rated 200V.

**Receiving end:** the LMR16030 EN pin tolerates 0–60V with a ~1.2V threshold (datasheet §6.1/§6.5), so no protection is needed. A per-node RC + Schmitt filter is cheap transient-noise insurance but not load-bearing (see §7 item 5).

**Why no separate RESET line:** power-cycling a node via ENABLE also resets its MCU, making ENABLE a strict superset of a dedicated `NRST` bus — the only thing lost is resetting the MCU *without* dropping node power, which isn't needed here (not battery-powered; the fuse/inrush design already tolerates power-cycling). Revisit if a glitch-free soft-reset path is ever needed.

**Hard-wired through:** ENABLE passes straight through each node's in/out RJ45, not buffered or re-driven by the MCU, so a master kill works even against hung firmware. Trade-off of the per-node pull-up: if the ENABLE conductor breaks mid-chain, downstream nodes self-enable and the master loses kill authority over that segment — unlikely, since the bonded green pair shares the RJ45 with power, so such a break usually drops that segment's power anyway.

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

## 6. Node core schematic — STM32G030F6P6, transceiver, indicators

This section is the reusable node front-end: MCU support parts, the RS-485 transceiver, status LEDs, and the reset/user buttons. Application-specific pins (servo PWM, sensor front-ends, the `ControllerNode`↔`Thermostat` link) are left to the module specs.

### 6.1 STM32G030F6P6 support components

TSSOP20: a **single combined `VDD/VDDA` pin (4)** and **single `VSS/VSSA` (5)** — no separate analog supply, no `VREF+` pin (ADC reference is `VDD` directly).

| Net | Parts | Notes |
|---|---|---|
| `VDD` (pin 4) | 100 nF + 1 µF at the pin, plus 4.7 µF bulk on the local 3V3 rail | Standard STM32G0 decoupling; keep the 100 nF loop tight. |
| ADC reference | (none — it's `VDD`) | Matters for `TemperatureNode` if NTC-into-ADC is chosen: LDO ripple sets the temperature noise floor. Consider AP2112 (`C51118`) over XC6206 there. |
| `NRST` (pin 6) | 100 nF to GND + reset button (§6.4) | Internal ~40 kΩ pull-up present — no external pull-up. |
| `BOOT0` | nothing | Shares the `PA14`/SWCLK pad. Factory `nBOOT_SEL = 1` → boot source is the option byte, pad free as SWCLK, boots from flash. Leave default (SWD flashing means no UART-bootloader strap is needed). |
| Clock | **HSI16 only** | HSI16 is ±0.75 % trimmed, ±1 %/0–85 °C, ±2 % at −40 °C — fine for 115200 and up to ~250–460 k node-to-node. No HSE pins are bonded on TSSOP20 unless a crystal is routed onto `PC14/PC15` (pins 2/3), which costs `PB9` + `PC15` as GPIO. Footprint an 8 MHz crystal there **DNP**, populate only if the bus is later committed to ≥1 Mbit. |
| SWD | 5-pin header: `3V3` (Vtref), `SWDIO`=PA13 (18), `SWCLK`=PA14 (19), `NRST` (6), `GND` | No caps on SWDIO/SWCLK. Cortex-M0+ has **no SWO/ITM** — see §6.5. |

### 6.2 Pin assignment (TSSOP20)

Recommended — **no SYSCFG pad remap needed**, USART1 on port B + PA12:

| Pin | Name | Node-core use |
|---|---|---|
| 1 | PB7 | **USART1_RX** (AF0) ← transceiver RO |
| 2 | PB9/PC14 | free (HSE xtal DNP) |
| 3 | PC15 | free (HSE xtal DNP) |
| 4 | VDD/VDDA | 3V3 |
| 5 | VSS/VSSA | GND |
| 6 | NRST | reset button + 100 nF |
| 7–13 | PA0–PA6 | **application** (servo PWM, sensors, link) |
| 14 | PA7 | **activity LED** |
| 15 | PB0…/PA8 mux | **error LED** |
| 16 | PA11 | **user button** (`ErrorHandler` ack) |
| 17 | PA12 | **USART1_DE** (AF1) → transceiver DE+/RE |
| 18 | PA13 | SWDIO |
| 19 | PA14 | SWCLK |
| 20 | PB3/4/5/6 mux | **USART1_TX** = PB6 (AF0) → transceiver DI |

Pins 1, 15 and 20 each bond several GPIO pads to one physical pin — configure exactly one and leave the rest at reset default (analog-in). Pins 16/17 (`PA11[PA9]`/`PA12[PA10]`) can be remapped to PA9/PA10 via `SYSCFG_CFGR1` `PA11_RMP`/`PA12_RMP`; this plan does not use that.

> **Firmware follow-up:** `Hal::UartPins` applies one `alternateFunction` to TX, RX *and* DE, but on this package **no USART1 pin combination shares a single AF** (recommended plan: TX/RX are AF0, DE is AF1). `UartPins` needs a per-pin AF field (or the DE AF set separately) before bring-up.

### 6.3 RS-485 transceiver

**Part: MAX3485CSA-JSM (JSMSEMI)** — LCSC `C6395158`, SOP-8. 3.3 V half-duplex RS-485, −40…+85 °C, 12 Mbps, ±8 kV HBM / ±15 kV IEC-air ESD. A 3.3 V part is *required*: at 3.3 V rail, a 5 V transceiver would drive its RO output at 5 V into the MCU RX pin. Standard MAX485/MAX3485 SO-8 pinout (`1 RO · 2 /RE · 3 DE · 4 DI · 5 GND · 6 A · 7 B · 8 VCC`). Second source: HTCSEMI `HT83485ARZ`, LCSC `C2960978`.

**Driver-enable:** use **USART1's hardware DE output** — tie the transceiver's `DE` (3) and `/RE` (2) together and drive from `USART1_DE` (`UART_DE_POLARITY_HIGH`: high = drive, low = listen). The peripheral handles `DEAT`/`DEDT` assertion timing per frame; no `setEnable()`/`delay()` loop (which the old ATmega design needed).

```
 PB7  USART1_RX ───────────────  1 RO
 PB6  USART1_TX ───────────────  4 DI
 PA12 USART1_DE ──┬────────────  3 DE
                  └────────────  2 /RE
                  └── 10k ── GND         (reset-safe: driver stays off while the MCU pin is Hi-Z)
      3V3 ─ 100nF ─ GND ───────  8 VCC / 5 GND
                                 6 A ─[10Ω]─ A ─ RJ45 pin 4 (blue)
                                 7 B ─[10Ω]─ B ─ RJ45 pin 5 (blue)
```

The **10 kΩ pull-down on the DE/RE net is mandatory** — without it an unprogrammed or resetting node can float its driver on and fight the bus.

Per-node A/B passives, **fit only where noted, DNP elsewhere:**

| Part | Value | Populate |
|---|---|---|
| Termination | 120 Ω across A–B | only the two physical bus ends (DNP + jumper on every node) |
| Fail-safe bias | A→3V3, B→GND, ~560 Ω each | once on the whole bus (at MainController); MAX3485 is not true-fail-safe |
| ESD/surge | **SM712 RS-485 TVS** (SOT-23-3): **pin 1 → A, pin 2 → B, pin 3 → GND** (verified against the Bourns CDSOT23-SM712 datasheet — pin 3 is the common). Asymmetric −7 V/+12 V per line, matching the RS-485 window. LCSC `C5199207` (ElecSuper) or `C404012` (genuine Bourns). | every node, right at the RJ45; short traces, pin-3 ground straight to the connector-side ground/shield stitch |
| Series R | 10 Ω in each of A/B | optional, tames ringing/EMI |

**Common-mode range check (§3 ground-offset):** MAX3485 has the standard EIA-485 receiver common-mode range **−7 V…+12 V** (abs. max −7.5…+12.5). Same shared-GND-return exposure as ENABLE. Single-end-fed estimate was ~3.45 V normal / ~11.6 V worst-case stall (the stall figure right at the +12 V edge); the **both-ends power feed (§4) cut worst-case IR drop to ~1/4**, revising these to **~0.86 V / ~2.9 V** — comfortably inside the window with >4× margin. RS-485's common-mode range is silicon-fixed (no "drive it higher" lever like ENABLE had), so §4's fix was the only route, and it works. §7 items 1–2 still matter for fuse/PTC and connector sizing but are no longer signal-integrity-critical.

### 6.4 Status LEDs & buttons

Driven by `NodeLib`:

- `Node(ledPin, errorLedPin, buttonPin=nullopt)`.
- **Activity LED** (`ledPin`, pin 14) — lit while the node transmits its queued messages (`Node::flushQueue`), off when idle.
- **Error LED** (`errorLedPin`, pin 15) — handed to `NodeLib::ErrorHandler`, blinked at 1 Hz on error; if `recoverable` *and* a `buttonPin` was given, blinks until the button is pressed.
- **User button** (`buttonPin`, pin 16) — configured `InputPullUp`, so **active-low: wire button → pin → GND**, 100 nF across it for debounce, optional 100–330 Ω series. Internal ~40 kΩ pull-up is enough for an on-board button; add an external 10 kΩ if it's on a long lead.

All LED GPIOs are push-pull, **active-high** (`led.Write(true)` = lit). Wire each `pin → R → LED anode, cathode → GND`.

| Function | LCSC | Part | Vf | notes |
|---|---|---|---|---|
| red (error) | `C2286` | Hubei KENTO KT-0603R | 1.8–2.4 V | 0603, JLC Basic, ~2.5 M stock |
| green (activity) | `C916074` | TUOZHAN TZ-P2-0603YGTCS1 | 1.9–2.4 V | 0603, 570–575 nm yellow-green (works on 3.3 V, unlike a 525 nm green at ~3.1 V) |

**Series resistor: 330 Ω** for every indicator LED (red / green / any orange). `R = (3.3 − ~2.0) / I` → ~3.3–4 mA, well inside the 8 mA/pin guideline; the low-mcd parts need the current to be readable. 470 Ω if a softer indicator is wanted. Avoid blue/white/525 nm-green (Vf ≈ 3.1 V) on the 3.3 V GPIO rail entirely.

Node IDs come from `DETECTNODES`/`HELLOWORLD` discovery — **no DIP switch / address strap needed.**

### 6.5 Easy-to-forget checklist

1. **Debug output needs a UART pin** — Cortex-M0+ has no SWO; `Tools::Logger` is a weak no-op meant to be routed to a UART. Bring USART2 (PA2/PA3) to a 3-pin header on `TemperatureNode`/`MainController`. On `ControllerNode` both USARTs are used (bus + thermostat link) — plan for sharing.
2. **RJ45 straight-through nets** — 48 V (orange 1/2), GND (brown 7/8), A/B (blue 4/5), ENABLE (green 3/6) all pass in-jack → out-jack unbuffered. ENABLE also gets the per-node **1.5 MΩ pull-up to local 48 V** (§3) and feeds the buck EN pin.
3. **Input protection + regulators** per `Node-Bus-Power-Path-Spec.md` (PTC fuse, 48 V TVS, reverse diode, 47 µF/100 V bulk, LMR16030 buck, LDO, all caps).
4. **100 nF at every VCC pin** — MCU, transceiver, LDO.
5. **Reset-safe I/O** — every MCU output benign at POR; servo PWM pin externally pulled to the safe damper position.
6. **Test points:** 48 V, 5 V, 3V3, GND, A, B, debug-UART TX, SWD header.
7. **One solid ground pour**; for NTC-into-ADC keep the divider return near the MCU ground pin.

---

## 7. Open items — need your input before finalizing

1. **Real servo stall current** — §4's numbers use a generic placeholder. With both-ends power injection now committed (§4), this mainly affects fuse/PTC sizing and connector budget rather than signal integrity (§6's RS485 margin concern is resolved regardless, with >4x headroom even at the current placeholder stall estimate).
2. **Whether simultaneous full-stall across all 20 nodes is a realistic scenario** for your application (e.g. synchronized power-on homing) or a non-issue because servos move independently — no longer signal-integrity-critical now that both-ends injection (§4) resolved the RS485 margin question (§6) with comfortable headroom either way. Still relevant for fuse/PTC and connector-current sizing.
3. ~~**Cable run length per segment**~~ — partially resolved 2026-09-06: total bus length ~100m. No longer a factor for ENABLE (§3's per-node pull-up scheme removes the ground-offset exposure entirely). Per-segment breakdown and RS485 baud-rate implications not yet revisited against this number.
4. ~~**Final connector choice**~~ — resolved 2026-09-06: C7501838, two single-port jacks per node (see §5). Footprint still needs verification against the board layout once you're at PCB stage.
5. ~~**ENABLE line noise filtering**~~ — downgraded 2026-09-06: no longer load-bearing now that ENABLE drives at 48V (see §3) rather than 5V — the Schmitt-trigger + RC filter is optional insurance against transient noise, not a fix for a margin problem that no longer exists at this drive voltage. Add it if cheap, skip it if not without much consequence either way.
