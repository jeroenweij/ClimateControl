# Node Bus — Power Path Design Spec
### 48V → 5V (servo) → 3.3V (STM32G031F8P6) regulation chain

**Status:** Draft — component selection locked; RT resistor value and exact input-cap stock pending datasheet/BOM-time verification (see open items)
**Companion docs:** `RS485-Node-Protocol-Spec-STM32G030.md` (wire protocol), `Node-Bus-Hardware-Design-Spec.md` (connector, pin assignment, 48V bus rationale)

---

## 1. Topology

```
RJ45 (48V, RS485) → [Input protection] → [Buck 48V→5V] → 5V servo rail
                                                 │              │
                                          (EN pin)◄── Enable   └→ [LDO 5V→3.3V] → 3.3V MCU rail
                                                line (from RJ45)
```

Two regulation stages per node:
1. **48V → 5V** synchronous-looking but actually **non-synchronous** buck (single integrated high-side FET + external catch diode) — feeds the servo.
2. **5V → 3.3V** linear regulator (LDO) — feeds the STM32G031F8P6.

The shared **ENABLE** control line (carried on the RJ45 green pair per the hardware spec) is wired into the buck's **EN pin**. See §5 for what this decision does and doesn't cover.

---

## 2. Stage 1 — Input protection (48V rail, per node)

Order from the RJ45: **fuse → TVS → reverse-polarity diode → bulk cap → buck.**

| Function | Part | JLC # | Rating | Rationale |
|---|---|---|---|---|
| Resettable fuse | Littelfuse SMD050F-2 | **C428837** | 60V, 500mA hold / 1A trip | Sits above normal per-node bus current (~70–240mA per earlier bus-current calculation) but trips before a fault pulls significant current from the shared 48V rail |
| Surge/ESD TVS | SMAJ58A, unidirectional | **C2980408** | Vrwm 58V, Vc ~94V @ 4.3A, 400W, SMA | Cathode → 48V (post-fuse), anode → GND. Clamps cable-borne transients; on a sustained overvoltage it conducts and lets F1 trip. **Assumes the 48V supply is regulated near 48V** — if the bus can genuinely reach the 57V top of §3's input range, step to SMAJ60A/64A. Vc ~94V exceeds the LMR16030's 60V rating, so this is adequate for fast ESD (caps + fuse absorb it) but not a rigorous lightning-surge design — acceptable for an indoor plastic-enclosure system. |
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
| Internal reference | 0.75V (confirmed from TI datasheet) |
| Topology | **Non-synchronous** — integrated high-side FET only, requires external catch diode |
| Stock (at last check) | 22,000+ units |

**Output voltage equation:** `Vout = 0.75V × (1 + Rfbt / Rfbb)`

| Component | Value | JLC # | Notes |
|---|---|---|---|
| Catch diode | SK36A, 60V 3A Schottky | **C492931** | Matched to IC's full 3A rating |
| Inductor | 15µH, 4.5A rated / 8A saturation, 27mΩ DCR, shielded, SXN SMDRI127-150MT | **C40000** | Locked in 2026-09-06 — original pick (`C6078745`) was out of stock. This replacement beats the original spec on every axis (more rated/saturation current headroom, lower DCR), at the cost of a larger 12.3×12.3mm case — not yet checked against board space since the PCB isn't laid out yet. Rounding up from the ~10µH textbook value at 500kHz/30% ripple still applies here. |
| Feedback low-side resistor (Rfbb) | 10.0kΩ, **1%** | commodity | Within TI's recommended 10k–100k range. Keep 1% — with Rfbt at 0.5% this resistor dominates Vout accuracy. |
| Feedback high-side resistor (Rfbt) | 56.9kΩ | commodity | Gives Vout ≈ **5.02V**. Decided 2026-09-07: aim slightly *above* 5.0V. The servo rail tolerates up to 6V, so exact trim doesn't matter — anywhere ~5.0–5.5V is fine, and no reason to risk landing under 4.8V. |
| Input HF capacitors | 2× 2.2µF, 100V, X7R, 1206 (SAMWHA CS3216X7R225K101NRI) | **C920964** | Locked in 2026-09-06 — the originally-flagged placeholder (`C49326820`) turned out to be out of stock, confirmed when checking at BOM time as this doc anticipated. This replacement is 1206 rather than 1210 (smaller footprint, same electrical spec) — deliberate, not a typo. 29,245 in stock at last check. |
| Input bulk capacitor | **47µF, 100V** aluminium electrolytic (~8mm can) | commodity | Decided 2026-09-07, reduced from an earlier 220µF. 47µF is TI's LMR16030 reference-design bulk value; the 220µF was over-specced and its hot-plug inrush energy (~0.25J) was the reason §2 called for an inrush limiter. At 47µF that energy drops ~5× to ~50mJ. **Must be ≥100V** — the SMAJ58A bus TVS (§2) clamps at ~94V, so an 80V part is not safe. |
| Output capacitors | 2–3× 22µF, 25V, 1210 | commodity | Standard low-ESR ceramics |
| Bootstrap capacitor | 100nF, 25V, 0603 | commodity | For the internal high-side gate driver |
| Soft-start capacitor | ~47nF | commodity, optional | Slows startup inrush, complements upstream PTC/diode protection |
| RT resistor (switching frequency) | 49.9kΩ, 1% → sets fSW = 500kHz | commodity | Locked in 2026-09-06. 500kHz chosen to match the ripple assumption this doc's inductor sizing already used, and matches TI's own 5V/3A reference design (datasheet Table 8-1). RT value from datasheet Table 7-1 (typical fSW→RT table) and confirmed via Equation 5, `R_T(kΩ) = 42904 × f_SW(kHz)^-1.088` → 49.66kΩ calculated, 49.9kΩ standard value. |

---

## 4. Stage 3 — 5V → 3.3V LDO

**IC: XC6206P332MR** — JLC **C5446**, SOT-23-3

| Spec | Value |
|---|---|
| Output | Fixed 3.3V |
| Application | STM32G031F8P6 draws tens of mA — well within this part's range |
| Support components | ~1µF ceramic in and out (0603, commodity) |

**Alternative if independent gating is wanted:** AP2112K-3.3TRG1 — JLC **C51118**, same footprint class, adds an EN pin (see §5).

**MainController board only — the NINA-W152 does NOT share this rail.** It draws ~120 mA average (Wi-Fi TX, datasheet Table 13) with ~250–350 mA ms-peaks — far past the XC6206's "tens of mA" range, and sagging a shared rail would brown out the MCU. Give the NINA its own 3V3 LDO off the 5 V buck (≥500 mA, in a package that sheds ~0.3–0.5 W — MIC5504 / TLV75533 / AP7361C / RT9080), with a local 22 µF + 10 µF + 100 nF at its VCC pins. Populate on the MainController build, DNP on the TemperatureNode build (mutually exclusive with the 1-Wire front-end).

---

## 5. what ENABLE actually kills

The ENABLE line (from the RJ45 green pair, bonded both conductors, per the hardware spec §3) is wired to the **buck's EN pin** in this design. Since the 3.3V LDO is fed from the buck's 5V output, **ENABLE kills the entire node — MCU included.** This is also why the separate RESET/`NRST` bus was dropped (hardware spec §3, decided 2026-09-06): a power-cycle via ENABLE resets the MCU too, making a dedicated reset line redundant for this design.

**Drive scheme:** the ENABLE net sits at 48V (bus line voltage), not a logic level. Each node pulls it up to its *own* local 48V through a 1.5MΩ resistor (`C2933600`, see hardware spec §3); the master disables the whole bus with a single low-side N-MOSFET to GND, no pull-up at the master. Enabled is the default state. Putting the pull-up per-node makes the "enabled" level independent of the shared line and its IR/ground-offset — see hardware spec §3 for the full rationale. The LMR16030 EN pin tolerates 0–60V with a ~1.2V threshold (datasheet §6.1/§6.5), so 48V needs no extra protection at the receiving end.

**Optional addition at the receiving end (each node, before the EN pin):** a Schmitt-trigger buffer plus a small RC low-pass filter, to reject noise coupled in from the local buck's own 500kHz switching and from neighboring nodes' converters on the same cable run. Not load-bearing — cheap insurance against transient chatter, not a fix for anything broken.

---

## 6. Open items — need your input / datasheet lookup before finalizing

1. ~~**RT resistor value**~~ — resolved 2026-09-06: 500kHz / 49.9kΩ, see §3.
2. ~~**Input capacitor stock**~~ — resolved 2026-09-06: switched to C920964 (see §3), the original C49326820 was confirmed out of stock.
3. ~~**Fine-tune Rfbt**~~ — resolved 2026-09-07: 56.9kΩ → ~5.02V, deliberately a touch above 5.0V; servo rail tolerates 6V so no tight trim needed (§3).
