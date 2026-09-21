# Node Bus — Power Path Design Spec
### 48V → 5V (servo) → 3.3V (STM32G031F8P6) regulation chain

**Companion docs:** `RS485-Node-Protocol-Spec-STM32G030.md` (wire protocol), `Node-Bus-Hardware-Design-Spec.md` (connector, pin assignment, 48V bus rationale)

---

## 1. Topology

```
RJ45 (48V, RS485) → [Input protection] → [Buck 48V→5V] → 5V rail ─┬─[servo load switch]─► servo   (ControllerNode)
                                                 │                │        ▲
                                          (EN pin)◄── Enable      │   MCU GPIO, OFF by default (§3.1)
                                                line (from RJ45)  └─→ [LDO 5V→3.3V] → 3.3V MCU rail
```

Two regulation stages per node:
1. **48V → 5V** synchronous-looking but actually **non-synchronous** buck (single integrated high-side FET + external catch diode) — feeds the servo (via the §3.1 load switch on a `ControllerNode`) and the LDO.
2. **5V → 3.3V** linear regulator (LDO) — feeds the STM32G031F8P6.

The shared **ENABLE** control line (carried on the RJ45 green pair per the hardware spec) is wired into the buck's **EN pin**. See §5 for what this decision does and doesn't cover.

---

## 2. Stage 1 — Input protection (48V rail, per node)

Order from the RJ45: **fuse → TVS → reverse-polarity diode → bulk cap → buck.**

| Function | Part | JLC # | Rating | Rationale |
|---|---|---|---|---|
| Resettable fuse | Littelfuse SMD050F-2 | **C428837** | 60V, 500mA hold / 1A trip | Sits above normal per-node bus current (~70–240mA per bus-current calculation) but trips before a fault pulls significant current from the shared 48V rail |
| Surge/ESD TVS | SMAJ58A, unidirectional | **C2980408** | Vrwm 58V, Vc ~94V @ 4.3A, 400W, SMA | Cathode → 48V (post-fuse), anode → GND. Clamps cable-borne transients; on a sustained overvoltage it conducts and lets the fuse trip. Assumes the 48V supply is regulated near 48V — if the bus can genuinely reach the 57V top of §3's input range, step to SMAJ60A/64A. Vc ~94V exceeds the LMR16030's 60V rating, so this is adequate for fast ESD but not a rigorous lightning-surge design — acceptable for an indoor plastic-enclosure system. |
| Reverse-polarity diode | SS26A / SK26, Schottky | **C908236** | 60V, 2A | Series diode protects against a mis-wired or reversed cable segment; ~0.5–0.7V drop is negligible against 48V |

**Hot-plug inrush:** handled by keeping the input bulk cap small (47µF, see §3) rather than an NTC or soft-start FET. At 47µF the inrush energy (~50mJ) is acceptable for the infrequent hot-plug of an indoor system. Revisit (add a series NTC or high-side P-FET soft-start) only if connector arcing is observed in the field, or if bulk capacitance has to grow.

---

## 3. Stage 2 — 48V → 5V buck converter

**IC: TI LMR16030PDDAR** — JLC **C90665**, SOIC-8 PowerPAD

| Spec | Value |
|---|---|
| Input range | 4.3–60V (covers 40–57V bus with margin) |
| Output | Adjustable via FB divider |
| Output current | 3A continuous |
| Internal reference | 0.75V |
| Topology | **Non-synchronous** — integrated high-side FET only, requires external catch diode |

**Output voltage equation:** `Vout = 0.75V × (1 + Rfbt / Rfbb)`

| Component | Value | JLC # | Notes |
|---|---|---|---|
| Catch diode | SK36A, 60V 3A Schottky | **C492931** | Matched to IC's full 3A rating |
| Inductor | 15µH, 4.5A rated / 8A saturation, 27mΩ DCR, shielded, SXN SMDRI127-150MT | **C40000** | Beats a 10µH textbook value at 500kHz/30% ripple on every axis (more rated/saturation current headroom, lower DCR), at the cost of a larger 12.3×12.3mm case. |
| Feedback low-side resistor (Rfbb) | **12.0kΩ, 1%** — `0603WAF1202T5E`, LCSC `C22790` (0603) | Within TI's recommended 10k–100k range. |
| Feedback high-side resistor (Rfbt) | **75.0kΩ, 1%** — LCSC `C23242` (0603) | Gives Vout ≈ **5.44V**. Both resistors are JLCPCB Basic parts, ordinary ±1%. Lands in the intended **5.0–5.5V window**: the real ceiling is not the servo (DS3225, `Node-Bus-Hardware-Design-Spec.md` §4 — 4.8–6.8V operating range on its own) but the shared-rail LDOs downstream — `RT9080-33GJ5` (this board's own 3.3V regulator, §4) is only specified up to **VIN = 5.5V** (Richtek DS9080-09 §12, "Recommended Operating Conditions"; 6.5V is the destructive absolute-max, not a target), and the Thermostat's `XC6206P332MR` sees the same rail over the link cable (`ControllerNode-Thermostat-Link-Spec.md` §3.1) with a ~6V recommended ceiling of its own. Do not chase the DS3225's higher-torque top end (6.8V) by raising the buck output. At ±1% on both resistors the worst-case-high corner is ~5.53V — ~30mV over RT9080's recommended ceiling, not its 6.5V absolute max; accepted as a low-probability corner case. |
| Input HF capacitors | 2× 2.2µF, 100V, X7R, 1206 (SAMWHA CS3216X7R225K101NRI) | **C920964** | 1206 rather than 1210 (smaller footprint, same electrical spec). |
| Input bulk capacitor | **47µF, 100V** aluminium electrolytic (~8mm can) | commodity | TI's LMR16030 reference-design bulk value; keeping it at 47µF (rather than an over-specced 220µF) keeps hot-plug inrush energy down to ~50mJ (§2). **Must be ≥100V** — the SMAJ58A bus TVS (§2) clamps at ~94V, so an 80V part is not safe. |
| Output capacitors | 2–3× 22µF, 25V, 1210 | commodity | Standard low-ESR ceramics |
| Bootstrap capacitor | 100nF, 25V, 0603 | commodity | For the internal high-side gate driver |
| Soft-start capacitor | ~47nF | commodity, optional | Slows startup inrush, complements upstream PTC/diode protection |
| RT resistor (switching frequency) | 49.9kΩ, 1% → sets fSW = 500kHz | commodity | Matches TI's own 5V/3A reference design (datasheet Table 8-1). RT value from datasheet Table 7-1 (typical fSW→RT table) and confirmed via Equation 5, `R_T(kΩ) = 42904 × f_SW(kHz)^-1.088` → 49.66kΩ calculated, 49.9kΩ standard value. |

### 3.1 Servo rail gating (ControllerNode only)

The servo is not wired straight to the 5V rail — it sits behind a **high-side load switch** the STM32 controls (`Board::ServoEnable`, `Node-Bus-Hardware-Design-Spec.md` §6.2).

- **Default off.** The switch is in the servo-off state whenever the MCU pin is Hi-Z — power-on, reset, unprogrammed board. The metal-geared damper actuator holds its last position unpowered.
- **On only during a move.** Firmware asserts `ServoEnable`, drives the PWM to the new target, waits for the actuator to settle, then de-asserts. A damper that is merely *holding* a position draws no servo current.
- **OTA / fault safe state.** `INodeHandler::PrepareForReset()` and any `ConnectionLost()` drive the damper to 50% (neutral airflow), then de-assert `ServoEnable` before resetting (`ControllerNode-Thermostat-Link-Spec.md` §5.1). A hung or resetting node applies no drive rather than latching its last PWM.

**Circuit:** high-side P-FET (source = 5V, drain = servo connector), gate pulled up to 5V, a small NMOS (2N7002) level-shifting the 3.3V GPIO onto the gate — GPIO high → NMOS on → P-FET on; GPIO Hi-Z → gate at 5V → P-FET off (reset-safe). The P-FET must carry the servo **stall** current continuously (jammed damper) — the servo is a DS3225 (`Node-Bus-Hardware-Design-Spec.md` §4): stall current 2.2–2.6A @5.0V → 2.8–3.2A @6.8V, interpolating to this board's actual 5.0–5.5V rail to **~2.6–2.8A** working. The `AO3401A` (~4A / <50mΩ) covers this with ~1.4× margin. Firmware bounds how long a stall condition is allowed to persist (park-and-de-energize, not indefinite hold, §3.1.1) — the P-FET's SOT-23 package makes continuous 2.8A a real thermal question (`I²R ≈ 0.39W`) if a jam were ever held rather than caught quickly. An integrated load switch (AP22802 / TPS22918-class, ≥3A, with slew + thermal control) remains the tidier option if board area allows. A 10–47µF reservoir on the switched side softens the start-of-move current step so it does not disturb the 5V rail or RS-485.

**Power-budget effect:** with the servo gated per node, the "all servos running" row in `Node-Bus-Hardware-Design-Spec.md` §4 (~12A at 5V / ~1.4A at 48V) is a **transient** bounded by how many dampers move at once, not a steady-state load. Resting bus current is ~20 × (MCU + transceiver) ≈ a few tens of mA total.

#### 3.1.1 Stall detection — servo current sense

The servo has no position-feedback wire (plain 3-wire PWM hobby servo), so a fixed move timeout can only *bound* how long a stalled/jammed damper is driven — it cannot *detect* the difference between "reached the setpoint" and "jammed for the whole timeout." Current sensing closes that gap, and shrinks the P-FET's worst-case stall exposure below the full move-timeout.

**Circuit — low-side sense, into the ADC:**

```
Servo GND (H2 pin 3) ──[R22, 12mΩ shunt]── SERVO_GND net ── system GND
                                │                              │
                              IN+ (pin 3) ── INA180A1 ── IN− (pin 4)
                                                │
                                              OUT (pin 1) ── PA0 (ADC_IN0), net "SENSE"
```

| Part | Value | LCSC | Notes |
|---|---|---|---|
| `R22` shunt | 12mΩ, ±1%, 1W, 1206 (`RALEC LR1206-21R012F4`) | `C154636` | At worst-case stall (~2.8A): ~34mV drop (negligible tax on the servo supply), ~94mW dissipated (<10% of rating). |
| `U17` sense amp | `INA180A1IDBVR(LX)`, gain 20V/V, SOT-23-5, **Pinout A** | `C48533472` | Compatible/second-source part, not genuine TI — irrelevant here since this is a threshold detector, not a calibrated ammeter. `INA180` ships in two different pinouts (A/B) depending on the part suffix — ours is `A1`, which is Pinout A (`1=OUT, 2=GND, 3=IN+, 4=IN-, 5=VS`). |

At the ~5.0–5.5V rail, ~2.6–2.8A stall gives `OUT ≈ 0.67V`; typical unloaded-move current (~0.3–0.8A) gives `OUT ≈ 0.07–0.19V` — a 3.5–9× spread, comfortably resolved on the 12-bit ADC.

**`VS` must be `+3.3V`, not `+5V`.** `PA0` is `FT_a` (5V-tolerant *digital* I/O with analog-switch function), and the general I/O input-voltage table does allow up to `Min(VDD+3.6, 5.5)V` — but the **ADC's own conversion range (`V_AIN`) is separately specified as `VSSA` to `VREF+`**, i.e. bounded by `VDDA` (≈3.3V here, no separate `VREF+` pin on this package), not extended by the digital FT tolerance. Powering the sense amp from `+5V` would let `OUT` swing toward ~5V under a fault condition, well outside the ADC's guaranteed range, even though normal operation never gets close (worst-case stall reading is only ~0.67V). Feeding it from the same `+3.3V` rail the ADC actually runs from removes the question entirely, at zero cost.

**Firmware approach (implemented, `Modules/ControllerNode/Damper.cpp`):** sample `PA0` (`Hal::Adc`, `Lib/HAL/Adc.h`) periodically while `ServoEnable` is asserted; require current to stay above a threshold for a sustained window (not a single sample) before declaring a stall — a raw instant threshold would false-trigger on the start-of-move current step the reservoir cap already exists to soften. On detection, de-energize immediately (faster than waiting out the full move timeout) and report a distinguishable fault code (`4 = stalled`) via the `DamperMode` endpoint (`Node-Message-Model-Spec.md` §3) rather than silently disabling — pushed both proactively on the main bus and to the paired Thermostat's display, and cleared on the next commanded move.

---

## 4. Stage 3 — 5V → 3.3V LDO

**IC: XC6206P332MR** — JLC **C5446**, SOT-23-3

| Spec | Value |
|---|---|
| Output | Fixed 3.3V |
| Application | STM32G031F8P6 draws tens of mA — well within this part's range |
| Support components | ~1µF ceramic in and out (0603, commodity) |

**Alternative if independent gating is wanted:** AP2112K-3.3TRG1 — JLC **C51118**, same footprint class, adds an EN pin (see §5).

**`ControllerNode` uses `RT9080-33GJ5` instead.** `ControllerNode` has only the one 3.3V rail (no `+3v3 P` peripheral load, §4.1 — that split only exists on the MainController/TemperatureNode variant), so `RT9080-33GJ5` (`C841192`, already on the BOM for other boards) is used there instead of a separate `XC6206P332MR` part number.

### 4.1 Second 3V3 rail — `+3v3 P` (peripheral rail, Main board)

The Main board carries a **second 3.3V regulator** feeding a separate `+3v3 P` net for the board's heavy / noisy peripheral, keeping it off the MCU's XC6206 rail (whose sag would brown out the MCU):

- **MainController build:** `+3v3 P` powers the NINA-W152 (`VCC` + `VCC_IO`). Wi-Fi TX draws ~120 mA average (datasheet Table 13) with ~250–350 mA ms-peaks — far past the XC6206's "tens of mA" range.
- **TemperatureNode build:** `+3v3 P` powers the DS18B20 1-Wire front-end (connectors + 4.7 kΩ pull-ups).

**The second regulator (`RT9080-33GJ5`) is populated on both build variants** — driving the ~3 mA 1-Wire load with a 600 mA LDO is harmless (no LDO minimum-load issue) and it avoids a DNP `+3v3 ↔ +3v3 P` bridge link. NINA and the 1-Wire front-end are never populated together.

**Part: RT9080-33GJ5** — LCSC `C841192`, TSOT-23-5. 600 mA (margin over the ~350 mA NINA TX peak), 75 dB PSRR @ 1 kHz (holds ~55 dB to 100 kHz — matters for the RF load), 2 µA Iq, stable with ceramics. Fed from the 5V buck. Tie `EN` to VIN (always on) → BOM is a 1 µF input cap plus the output bulk. **Output bulk: 22 µF near the LDO + ≥10 µF within a few mm of the NINA `VCC` pins** (the NINA sits in the opposite board corner from the LDO). Runners-up: TLV75533PDBVR (`C404027`, 500 mA, weaker 46 dB@100 kHz PSRR); AP7361C-33E (`C500795`, 1 A SOT-223) only if the enclosure runs hot. Not MIC5504 (`C4134807`) — 300 mA is under the TX peak.

---

## 5. What ENABLE actually kills

The ENABLE line (from the RJ45 green pair, bonded both conductors, per the hardware spec §3) is wired to the **buck's EN pin** in this design. Since the 3.3V LDO is fed from the buck's 5V output, **ENABLE kills the entire node — MCU included.** This is also why there is no separate RESET/`NRST` bus (hardware spec §3): a power-cycle via ENABLE resets the MCU too, making a dedicated reset line redundant for this design.

**Drive scheme:** the ENABLE net sits at 48V (bus line voltage), not a logic level. Each node pulls it up to its *own* local 48V through a 1MΩ resistor (`C17927`, see hardware spec §3); the master disables the whole bus with a single low-side N-MOSFET to GND, no pull-up at the master. Enabled is the default state. Putting the pull-up per-node makes the "enabled" level independent of the shared line and its IR/ground-offset — see hardware spec §3 for the full rationale. The LMR16030 EN pin tolerates 0–60V with a ~1.2V threshold (datasheet §6.1/§6.5), so 48V needs no extra protection at the receiving end.

**Optional addition at the receiving end (each node, before the EN pin):** a Schmitt-trigger buffer plus a small RC low-pass filter, to reject noise coupled in from the local buck's own 500kHz switching and from neighboring nodes' converters on the same cable run. Not load-bearing — cheap insurance against transient chatter, not a fix for anything broken.
