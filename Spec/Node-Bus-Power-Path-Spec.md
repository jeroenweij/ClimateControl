# Node Bus — Power Path Design Spec
### 48V → 5V (servo) → 3.3V (STM32G030F6P6TR) regulation chain

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
2. **5V → 3.3V** linear regulator (LDO) — feeds the STM32G030F6P6TR.

The shared **ENABLE** control line (carried on the RJ45 green pair per the hardware spec) is wired into the buck's **EN pin**. See §5 for what this decision does and doesn't cover.

---

## 2. Stage 1 — Input protection (48V rail, per node)

| Function | Part | JLC # | Rating | Rationale |
|---|---|---|---|---|
| Resettable fuse | Littelfuse SMD050F-2 | **C428837** | 60V, 500mA hold / 1A trip | Sits above normal per-node bus current (~70–240mA per earlier bus-current calculation) but trips before a fault pulls significant current from the shared 48V rail |
| Reverse-polarity diode | SS26A / SK26, Schottky | **C908236** | 60V, 2A | Series diode protects against a mis-wired or reversed cable segment; ~0.5–0.7V drop is negligible against 48V |

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
| Feedback low-side resistor (Rfbb) | 10.0kΩ, 1% | commodity | Within TI's recommended 10k–100k range |
| Feedback high-side resistor (Rfbt) | 56.2kΩ, 1% | commodity | Gives Vout ≈ 4.97V |
| Input capacitors | 2× 2.2µF, 100V, X7R, 1206 (SAMWHA CS3216X7R225K101NRI) | **C920964** | Locked in 2026-09-06 — the originally-flagged placeholder (`C49326820`) turned out to be out of stock, confirmed when checking at BOM time as this doc anticipated. This replacement is 1206 rather than 1210 (smaller footprint, same electrical spec) — deliberate, not a typo. 29,245 in stock at last check. |
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
| Application | STM32G030F6P6TR draws tens of mA — well within this part's range |
| Support components | ~1µF ceramic in and out (0603, commodity) |

**Alternative if independent gating is wanted:** AP2112K-3.3TRG1 — JLC **C51118**, same footprint class, adds an EN pin (see §5).

---

## 5. what ENABLE actually kills

The ENABLE line (from the RJ45 green pair, bonded both conductors, per the hardware spec §3) is wired to the **buck's EN pin** in this design. Since the 3.3V LDO is fed from the buck's 5V output, **ENABLE kills the entire node — MCU included.** This is also why the separate RESET/`NRST` bus was dropped (hardware spec §3, decided 2026-09-06): a power-cycle via ENABLE resets the MCU too, making a dedicated reset line redundant for this design.

**Drive level:** 48V (bus line voltage), not a logic level — changed 2026-09-06 from an earlier 5V-drive plan once the ground-offset math showed 5V didn't leave enough margin over a ~100m bus (see hardware spec §3 for the full analysis: ~3.45V worst-case-normal / ~11.6V worst-case-stall ground offset, vs. only ~3.6V of margin at 5V drive). At 48V drive the same worst-case offset leaves ~36V of margin. Driven from the master via a low-side N-MOSFET (onsemi BSS123, LCSC `C513249`) pulling the line to GND, with a 47kΩ pull-up to 48V — full BOM in hardware spec §3. The LMR16030's EN pin tolerates 0–60V with only a ~1.2V threshold (datasheet §6.1/§6.5), so 48V is within its rated range, no extra protection needed at the receiving end.

**Optional addition at the receiving end (each node, before the EN pin):** a Schmitt-trigger buffer plus a small RC low-pass filter, to reject noise coupled in from the local buck's own 500kHz switching and from neighboring nodes' converters on the same cable run. No longer load-bearing for margin now that the drive is 48V rather than 5V — cheap insurance against transient chatter, not a fix for anything broken.

---

## 6. Open items — need your input / datasheet lookup before finalizing

1. ~~**RT resistor value**~~ — resolved 2026-09-06: 500kHz / 49.9kΩ, see §3.
2. ~~**Input capacitor stock**~~ — resolved 2026-09-06: switched to C920964 (see §3), the original C49326820 was confirmed out of stock.
3. **Fine-tune Rfbt** if 4.97V vs. exactly 5.00V matters for your servo's tolerance.
