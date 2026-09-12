# Node Bus — Hardware Design Spec
### Power, connector, and node-schematic decisions for the STM32G031F8P6 RS485 node network

**Status:** Draft — decisions locked from design discussion; servo selected and cable-run distance confirmed, one open item remains (see §7)
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
| ENABLE pull-up | each node | **1MΩ** | LCSC `C17927` (`1206W4F1004T5E`, 1206 thick-film, 200V, 250mW, ±1%) — Basic part, same voltage/power rating as the original FOJAN pick it replaces (decided 2026-09-11) | To the node's local 48V input. ~48µA/node; ~0.96mA / ~46mW across a full 20-node bus. Feeds the buck EN pin through a 10kΩ series + 10nF-to-GND filter (noise + EN abs-max protection). |

**Pull-up value — 1MΩ per node** (revised 2026-09-11 from the original 1.5MΩ, to land on a JLCPCB Basic part instead of an Extended one — see part table above). Across 20 nodes this parallels to ~50kΩ: ~0.96mA total draw, and a ~0.5ms enable-edge RC against ~10nF of bus capacitance — still fine for a slow control line (if anything, a faster edge than the original 1.5MΩ pick). Lower values just waste power; higher values get noise-sensitive for little gain. The full 48V sits across the pull-up whenever the line is held low, so the resistor's package voltage rating (not its value) is the real constraint — the selected 1206 part is rated 200V, unchanged from the original pick.

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
| All 20 servos stalled simultaneously (worst case) | ~54 A | **~6.4 A** |

**Checked against the supplier's own spec sheet (§7 item 1, updated 2026-09-12):** their `DS3225` stalls at 2.2–2.6A @5.0V → 2.8–3.2A @6.8V — higher than the generic-datasheet estimate this table was originally built on. At this design's actual 5.0–5.5V working point that interpolates to **~2.6–2.8A/node**: 20 × 2.7A ≈ 54A (5V-side) → `I_48V = (54A × 5V)/(48V × 0.875) ≈ 6.4A`, up from the earlier ~4.7A (+~37%). Split across the both-ends power injection (§4 mitigation below) that's **~3.2A at each injection point** — now sitting at or just past the top of the "~2–3A safe headroom per hop" this section's own connector rating gives, where it previously sat comfortably mid-range. Still the extreme edge case §7 item 2 already questions the realism of (all 20 dampers stalling at the same instant) — worth a deliberate decision (accept the reduced margin for this edge case, or revisit connector/injection sizing) rather than carrying the old number forward silently.

**Decision:**
- Normal operation (~1.4A) is comfortably within a bonded-pair connector's safe range — no special handling needed for the typical case. The servo is per-node power-gated (`Node-Bus-Power-Path-Spec.md` §3.1), so both rows below are *transients* bounded by how many dampers move at once; the resting bus current is ~20 × (MCU + transceiver) ≈ tens of mA. The both-ends feed below is headroom for a coordinated multi-damper move.
- Worst-case simultaneous full-stall (~6.4A, updated 2026-09-12 from ~4.7A per the supplier's confirmed servo stall current above) can exceed the ~2–3A single-hop connector budget.
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

## 6. Node core schematic — STM32G031F8P6, transceiver, indicators

This section is the reusable node front-end: MCU support parts, the RS-485 transceiver, status LEDs, and the reset/user buttons. Application-specific pins (servo PWM, sensor front-ends, the `ControllerNode`↔`Thermostat` link) are left to the module specs.

**MCU locked in 2026-09-08: STM32G031F8P6.** Drop-in for the earlier STM32G030F6P6 — identical TSSOP20 pinout, same 64 MHz Cortex-M0+ / 8 KB SRAM, wider 1.7–3.6 V range. Gains: 64 KB flash (was 32 KB — room for a bus-resident DFU bootloader + the MainController's NINA-W152 driver), `LPUART1`, `RTC` + backup registers (a wear-free "enter bootloader" flag, and an optional wall-clock if an LSE crystal is fitted), `TIM2` (32-bit), 7 DMA channels. No board/layout change; schematic symbol + BOM line + HAL device define (`STM32G031xx`) + the future linker script (64 KB flash, G031 vector table) are the only follow-ups. Footprint stays compatible with STM32G031F6P6 (32 KB) as a cost-down fallback.

### 6.1 STM32G031F8P6 support components

TSSOP20: a **single combined `VDD/VDDA` pin (4)** and **single `VSS/VSSA` (5)** — no separate analog supply, no `VREF+` pin (ADC reference is `VDD` directly).

| Net | Parts | Notes |
|---|---|---|
| `VDD` (pin 4) | 100 nF + 1 µF at the pin, plus 4.7 µF bulk on the local 3V3 rail | Standard STM32G0 decoupling; keep the 100 nF loop tight. |
| ADC reference | (none — it's `VDD`) | Matters for `TemperatureNode` if NTC-into-ADC is chosen: LDO ripple sets the temperature noise floor. Consider AP2112 (`C51118`) over XC6206 there. |
| `NRST` (pin 6) | 100 nF to GND + reset button (§6.4) | Internal ~40 kΩ pull-up present — no external pull-up. |
| `BOOT0` | nothing | Shares the `PA14`/SWCLK pad. Factory `nBOOT_SEL = 1` → boot source is the option byte, pad free as SWCLK, boots from flash. Leave default (SWD flashing means no UART-bootloader strap is needed). |
| Clock | **HSI16 only** | HSI16 is ±0.75 % trimmed, ±1 %/0–85 °C, ±2 % at −40 °C — fine for the **250 000 baud** bus (§7 item 3): an indoor duct/wall sees ~10–40 °C where HSI16 holds ≈ ±1 %, so two nodes are ~±2 % apart, inside the async-UART framing budget — and 250 000 is an exact integer divisor of the USART clock, so the generator adds no error on top. This is why 1 Mbit is not used. Pins 2/3 (`PB9/PC14-OSC32_IN`, `PC15-OSC32_OUT`) can instead take a **32.768 kHz LSE crystal** for the G031 RTC (wall-clock for MainController schedules). On the MainController board `PB9` is the bus-disable output (`RESET_NODES`), so LSE there would need it relocated; on Node/Thermostat boards pins 2/3 are free. Footprint LSE **DNP** on all boards for now. |
| SWD | 5-pin header: `3V3` (Vtref), `SWDIO`=PA13 (18), `SWCLK`=PA14 (19), `NRST` (6), `GND` | No caps on SWDIO/SWCLK. Cortex-M0+ has **no SWO/ITM** — see §6.5. |

### 6.2 Pin assignment (TSSOP20)

Recommended — **no SYSCFG pad remap needed**, USART1 on port B + PA12:

| Pin | Name | Node-core use |
|---|---|---|
| 1 | PB7 | **USART1_RX** (AF0) ← transceiver RO  (Thermostat: **I2C1_SDA** AF6) |
| 2 | PB9/PC14 | free — or **RESET_NODES** bus-disable output (MainController); LSE xtal DNP |
| 3 | PC15 | free (LSE xtal DNP) |
| 4 | VDD/VDDA | 3V3 |
| 5 | VSS/VSSA | GND |
| 6 | NRST | reset button + 100 nF |
| 7–13 | PA0–PA6 | **application** — see per-board split below |
| 14 | PA7 | **activity LED** |
| 15 | PB0…/PA8 mux | **error LED** |
| 16 | PA11 | **user button** (`ErrorHandler` ack) |
| 17 | PA12 | **USART1_DE** (AF1) → transceiver DE+/RE |
| 18 | PA13 | SWDIO |
| 19 | PA14 | SWCLK |
| 20 | PB3/4/5/6 mux | **USART1_TX** = PB6 (AF0) → transceiver DI  (Thermostat: **I2C1_SCL** AF6) |

Pins 1, 15 and 20 each bond several GPIO pads to one physical pin — configure exactly one and leave the rest at reset default (analog-in). Pins 16/17 (`PA11[PA9]`/`PA12[PA10]`) can be remapped to PA9/PA10 via `SYSCFG_CFGR1` `PA11_RMP`/`PA12_RMP`; this plan does not use that.

**Per-board use of pins 7–13 (PA0–PA6):**

| Pin | MainController | ControllerNode ("Node" board) | TemperatureNode | Thermostat |
|---|---|---|---|---|
| 7  PA0  | USART2_CTS (AF1) ← NINA RTS | **servo stall-current sense** (`ADC_IN0`) — decided 2026-09-12, `Node-Bus-Power-Path-Spec.md` §3.1 | — | — |
| 8  PA1  | USART2_RTS (AF1) → NINA CTS | link DE (AF1) | — | link DE (AF1) |
| 9  PA2  | USART2_TX (AF1) → NINA | link TX (USART2, AF1) | — | link TX (USART2, AF1) |
| 10 PA3  | USART2_RX (AF1) ← NINA | link RX (USART2, AF1) | — | link RX (USART2, AF1) |
| 11 PA4  | — | free (bit-bang debug-TX candidate) | 1-Wire #2 | — |
| 12 PA5  | — | **servo enable** (GPIO out, off by default — `Node-Bus-Power-Path-Spec.md` §3.1) | 1-Wire #1 | — |
| 13 PA6  | NINA RESET_N (open-drain out) | servo PWM (TIM3_CH1, AF1) | — | — |

MainController + TemperatureNode are two populate variants of one PCB (the 48 V injection front-end and NINA are DNP on the TemperatureNode build, the 1-Wire front-end DNP on the MainController build). MainController has no spare hardware UART for a debug console (USART1 = bus, USART2 = NINA). The G031's `LPUART1` does not help: on TSSOP20 its TX/RX only reach PA2/PA3 (the PB10/PB11 and PC0/PC1 options are not bonded), i.e. the same pins as USART2. Bit-bang `Tools::Logger` on a free pin (PC15, PA4, PA5) or drop the console — **verify the LPUART1 AF map against DS12992 before relying on this.**

> **Firmware:** the pin map lives in `Software/Lib/Board/BoardPins.h` (single source of truth). `Hal::UartPins` carries a per-pin AF (`Board::BusUart` = PB6/PB7 at AF0, PA12 at AF1). Pin 15's error-LED pad is picked as **PB0** — configure only that one.

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
| Fail-safe bias | A→3V3, B→GND, ~560 Ω each | once on the whole bus (at MainController); MAX3485 is not true-fail-safe |
| ESD/surge | **SM712 RS-485 TVS** (SOT-23-3): **pin 1 → A, pin 2 → B, pin 3 → GND** (verified against the Bourns CDSOT23-SM712 datasheet — pin 3 is the common). Asymmetric −7 V/+12 V per line, matching the RS-485 window. LCSC `C5199207` (ElecSuper) or `C404012` (genuine Bourns). | every node, right at the RJ45; short traces, pin-3 ground straight to the connector-side ground/shield stitch |
| Series R | 10 Ω in each of A/B | optional, tames ringing/EMI |

**Termination — decided 2026-09-11: no on-board footprint, no per-node DNP/jumper.** 120 Ω A–B termination is provided by a **plug-in RJ45 terminator** (120 Ω across pins 4/5) inserted into the spare "out" jack of whichever node is physically last on the chain. This drops the per-node DNP-resistor-plus-jumper scheme entirely — every node's board is identical here, and moving/extending the bus end just means moving the terminator plug, not reworking a board.

**Common-mode range check (§3 ground-offset):** MAX3485 has the standard EIA-485 receiver common-mode range **−7 V…+12 V** (abs. max −7.5…+12.5). Same shared-GND-return exposure as ENABLE. Single-end-fed estimate was ~3.45 V normal / ~11.6 V worst-case stall (the stall figure right at the +12 V edge); the **both-ends power feed (§4) cut worst-case IR drop to ~1/4**, revising these to **~0.86 V / ~2.9 V** — comfortably inside the window with >4× margin. RS-485's common-mode range is silicon-fixed (no "drive it higher" lever like ENABLE had), so §4's fix was the only route, and it works. §7 items 1–2 still matter for fuse/PTC and connector sizing but are no longer signal-integrity-critical.

### 6.4 Status LEDs & buttons

Driven by `NodeLib`:

`Node`/`NodeMaster` take no pin arguments — they use `Board::ActivityLed` / `Board::ErrorLed` / `Board::UserButton` directly.
- **Activity LED** (`Board::ActivityLed`, PA7 / pin 14) — lit while the node transmits its queued messages (`Node::flushQueue`), off when idle.
- **Error LED** (`Board::ErrorLed`, PB0 / pin 15) — driven by `NodeLib::ErrorHandler`, blinked at 1 Hz on error; if `recoverable`, blinks until the user button is pressed.
- **User button** (`Board::UserButton`, PA11 / pin 16) — configured `InputPullUp`, so **active-low: wire button → pin → GND**, 100 nF across it for debounce, optional 100–330 Ω series. Internal ~40 kΩ pull-up is enough for an on-board button; add an external 10 kΩ if it's on a long lead.

All LED GPIOs are push-pull, **active-high** (`led.Write(true)` = lit). Wire each `pin → R → LED anode, cathode → GND`.

| Function | LCSC | Part | Vf | notes |
|---|---|---|---|---|
| red (error) | `C2286` | Hubei KENTO KT-0603R | 1.8–2.4 V | 0603, JLC Basic, ~2.5 M stock |
| green (activity) | `C916074` | TUOZHAN TZ-P2-0603YGTCS1 | 1.9–2.4 V | 0603, 570–575 nm yellow-green (works on 3.3 V, unlike a 525 nm green at ~3.1 V) |

**Series resistor: 330 Ω** for every indicator LED (red / green / any orange). `R = (3.3 − ~2.0) / I` → ~3.3–4 mA, well inside the 8 mA/pin guideline; the low-mcd parts need the current to be readable. 470 Ω if a softer indicator is wanted. Avoid blue/white/525 nm-green (Vf ≈ 3.1 V) on the 3.3 V GPIO rail entirely.

Node IDs are **factory-provisioned in flash** (`Node-Flash-Layout-and-Bootloader-Spec.md` §6.3) and reported to the master in the `Announce` reply during discovery — **no DIP switch / address strap needed.**

### 6.5 Easy-to-forget checklist

1. **Debug output needs a UART pin** — Cortex-M0+ has no SWO; `Tools::Logger` is a weak no-op meant to be routed to a UART. Bring USART2 (PA2/PA3) to a 3-pin header on `TemperatureNode`/`MainController`. On `ControllerNode` both USARTs are used (bus + thermostat link) — plan for sharing.
2. **RJ45 straight-through nets** — 48 V (orange 1/2), GND (brown 7/8), A/B (blue 4/5), ENABLE (green 3/6) all pass in-jack → out-jack unbuffered. ENABLE also gets the per-node **1.5 MΩ pull-up to local 48 V** (§3) and feeds the buck EN pin.
3. **Input protection + regulators** per `Node-Bus-Power-Path-Spec.md` (PTC fuse, 48 V TVS, reverse diode, 47 µF/100 V bulk, LMR16030 buck, LDO, all caps).
4. **100 nF at every VCC pin** — MCU, transceiver, LDO.
5. **Reset-safe I/O** — every MCU output benign at POR. On a `ControllerNode` the **servo enable** (PA5) defaults off with the pin Hi-Z, so the servo is unpowered at POR / on an unprogrammed board (`Node-Bus-Power-Path-Spec.md` §3.1); keep a light external pull on the servo PWM pin toward the safe position as secondary insurance.
6. **Test points:** 48 V, 5 V, 3V3, GND, A, B, debug-UART TX, SWD header.
7. **One solid ground pour**; for NTC-into-ADC keep the divider return near the MCU ground pin.

---

## 7. Open items — need your input before finalizing

1. ~~**Real servo stall current**~~ — resolved 2026-09-11, **superseded 2026-09-12 with supplier-confirmed numbers**: actuator selected, **DSSERVO `DS3225`**, 25 kg·cm metal-gear digital servo (coreless, "waterproof" housing, ~40×20×40.5 mm, 67 g, 180° version). Generic datasheet listings quoted ~1.9A @5.0V; the actual supplier's own spec sheet for the units being bought gives **operating voltage 4.8–7.2V, stall current 2.2–2.6A @5.0V → 2.8–3.2A @6.8V** — noticeably higher than the generic figure. **Recommended operating point unchanged: run it at this board's existing 5.0–5.5V rail, not higher** (`Node-Bus-Power-Path-Spec.md` §3's Rfbt row — the ceiling is the shared-rail LDOs, not the servo). Interpolating their two points to our actual 5.0–5.5V rail: **~2.6–2.8A is the working stall-current number**, not ~2.0A. §3.1 of the power-path spec and §4's connector-budget table below are both updated for this.
2. **Whether simultaneous full-stall across all 20 nodes is a realistic scenario** for your application (e.g. synchronized power-on homing) or a non-issue because servos move independently — no longer signal-integrity-critical now that both-ends injection (§4) resolved the RS485 margin question (§6) with comfortable headroom either way. Still relevant for fuse/PTC and connector-current sizing.
3. ~~**Cable run length per segment / RS485 baud rate**~~ — total bus length ~100 m. Not a factor for ENABLE (§3's per-node pull-up scheme removes the ground-offset exposure entirely). **Baud: 250 000** (`RS485-Node-Protocol-Spec-STM32G030.md` §9) — an exact integer USART divisor (zero baud-generator error, which matters because the HSI16 clock spread already spends most of the async-UART budget); length·rate = 2.5×10⁷ bit·m/s sits deep inside the safe region for 100 m of terminated twisted pair with 20 lightly-loaded stubs. 500 000 (also exact) is the next step only if bench-validated on the real cable; 1 Mbit is not recommended — too little margin against HSI16 spread + loaded edges for a duct-buried bus.
4. ~~**Final connector choice**~~ — resolved 2026-09-06: C7501838, two single-port jacks per node (see §5). Footprint still needs verification against the board layout once you're at PCB stage.
5. ~~**ENABLE line noise filtering**~~ — downgraded 2026-09-06: no longer load-bearing now that ENABLE drives at 48V (see §3) rather than 5V — the Schmitt-trigger + RC filter is optional insurance against transient noise, not a fix for a margin problem that no longer exists at this drive voltage. Add it if cheap, skip it if not without much consequence either way.
