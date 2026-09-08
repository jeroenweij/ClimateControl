# TemperatureNode — Design Spec

**Status:** Draft — role confirmed 2026-09-06, sensor chosen (§4), value encoding / report rate still open (§5)
**Companion docs:** `RS485-Node-Protocol-Spec-STM32G030.md` (wire protocol), `Node-Bus-Hardware-Design-Spec.md` / `Node-Bus-Power-Path-Spec.md` (shared bus power/connector), `Software-Architecture-Spec.md` (module map)

---

## 1. Confirmed role

`TemperatureNode` is a slave node on the main RS485 bus (same bus, same `NodeLib` protocol, same physical/power design as `ControllerNode` — see the hardware and power-path specs). It measures **incoming and outgoing air temperature in the main ducting, close to the outside HVAC unit**.

That's two sensing channels per node:

| Channel | Measures |
|---|---|
| "incoming" | Air temperature entering the HVAC unit from the house (return air) |
| "outgoing" | Air temperature leaving the HVAC unit into the house (supply air) |

Both channels are periodic `VALUE` reports, analogous to the old `ANALOG_IN` channel behavior in `Channel::Loop()` (debounced, only sends on change or `forceUpdate`) — same pattern, new value type per §7 of the protocol spec (variable-length `DATA` instead of a single `uint8_t`, since temperature almost certainly needs more range/precision than 0–255 raw).

---

## 2. Relationship to ControllerNode

`TemperatureNode` only *measures* — it does not drive a damper and has no `Thermostat` link. Per `MainController-Spec.md` §2, what consumes these readings (closed-loop control vs. supervisory logging) is still open; `TemperatureNode`'s job is just to get accurate, timely readings onto the bus.

---

## 3. Physical placement implications

Mounted in ducting near the outside unit means:

- Likely more thermally extreme / less climate-controlled environment than an indoor `ControllerNode` — sensor and enclosure need a wider operating range than a typical indoor part.
- Depending on duct material and insertion method, may need a probe-style sensor (inserted through the duct wall) rather than an ambient/board-mounted sensor.
- Shares the same 48V-in/RJ45 power and RS485 physical layer as every other node on the bus (`Node-Bus-Hardware-Design-Spec.md` §2–§6). It is a **populate variant of the Main board** (shared with `MainController`): the 48 V-injection front-end and the NINA-W152 are DNP, the DS18B20 front-end (§4.1) is populated, and the second `+3v3 P` LDO (RT9080) is populated on both variants (`Node-Bus-Power-Path-Spec.md` §4.1). No separate power design needed unless the near-duct location demands a different enclosure ingress rating (mechanical concern, flagged for that design).

---

## 4. Sensor — DS18B20 on a 1-Wire probe lead

Chosen 2026-09-07: **DS18B20**, 1-Wire, mounted in a stainless probe on a short lead.

**Why DS18B20 over an NTC:**
- **One bus, both channels.** 1-Wire is a bus — the "incoming" and "outgoing" probes sit on the *same three wires* (VDD/GND/DQ) and are read individually by their 64-bit ROM ID. An NTC would need a separate ADC channel + bias resistor + calibration table per probe.
- No ADC calibration / Steinhart-Hart table; digital reading is immune to lead-length and analog noise.
- −55…+125 °C, ±0.5 °C — covers hot supply air and sub-zero return/outdoor air with margin.
- A pre-made **waterproof stainless-tube probe** (SS tube + ~0.5 m lead) handles condensation on the cooling side and gives a rugged in-airflow probe. e.g. LCSC `C843306`, or a bare DS18B20 (`C376006`, UMW, TO-92) potted on a pigtail.

### 4.1 On-board front-end (TemperatureNode populate variant of the Main board)

The probes are off-board cable assemblies. As built (`Hardware/Main/*_PCB1_1_2026-09-08`), each probe gets its **own single-drop 1-Wire line** rather than sharing one bus — costs a second GPIO + pull-up, buys simpler firmware (one device per line, no ROM search, Skip-ROM reads).

| Ref (board) | Part | Purpose |
|---|---|---|
| U5, U12 | 3-pos 5.0 mm screw terminal (BX-DG01V-5.0-3P5), one per probe | land the bare probe leads |
| R10, R15 | 4.7 kΩ, 0402 | DQ → `+3v3 P` pull-up — **one per line** |
| R16, R17 | 100 Ω, 0402 | series in each DQ line, MCU-pin ↔ connector — pin protection |
| D7, D8 | PESD3V3L1BA single-line ESD diode to GND | DQ leaves the enclosure |

```
 +3v3 P ──┬── R10 (4.7k) ──┐                 +3v3 P ──┬── R15 (4.7k) ──┐
          │                │                          │                │
 PA5 ──[R16 100R]──────────●── ONEWIRE ── U5-2         PA4 ──[R17 100R]─●── ONEWIRE2 ── U12-2
                           └── D7 ─┴─ GND                               └── D8 ─┴─ GND

 +3v3 P ── U5-1, U12-1     (probe VDD — DS18B20 at 3.3 V, NOT 5 V)
 GND    ── U5-3, U12-3
```

- **DQ pins: PA5 (`Board::OneWire1`, pin 12) and PA4 (`Board::OneWire2`, pin 11).** Plain GPIO bit-bang, pin as open-drain (drive low / release, the 4.7 kΩ pulls high). *(Not PA0 — that is now the NINA `USART2_CTS` on the MainController variant of this shared board.)*
- Probe VDD comes from **`+3v3 P`** (the RT9080 peripheral rail, always populated — `Node-Bus-Power-Path-Spec.md` §4.1), not the MCU's XC6206 rail.
- Pull-up + ESD diode on the connector side of the 100 Ω series R.

**Firmware:** 1-Wire bit-bang on PA4 and PA5 independently, open-drain, timing-critical slots with interrupts briefly masked — ~1–2 KB. One device per line → Skip-ROM `CONVERT`/`READ SCRATCHPAD`, no ROM search; the "incoming"/"outgoing" mapping is just which connector (PA5 = incoming, PA4 = outgoing, or per stored config). DS18B20's native `int16` in 1/16 °C is a clean bus value format.

### 4.2 Probe selection checklist

Any pre-made "DS18B20 waterproof stainless probe" (SS tube ~6×50 mm, 3-wire, 0.5–1 m lead) works — e.g. LCSC `C843306`, or the ubiquitous AliExpress SS-probe assemblies, or a bare `C376006` (UMW, TO-92) potted on a pigtail. Before bulk-ordering:

- **Verify a sample is a working DS18B20** — ROM family code `0x28`, CRC passes, tracks a reference thermometer. "Original DS18B20" claims are often a compatible die, not Maxim/ADI; fine for duct trend monitoring, not a traceable ±0.5 °C guarantee.
- **3-wire, not parasitic.**
- **Don't trust the wire colours** — cheap probes' listings contradict themselves; meter each of the three wires before wiring.
- **Cable jacket temp rating** — assume PVC (~70–80 °C) unless stated. Fine on the return-air side; use a silicone lead if a probe will sit in hot supply air.
- Plain tube (no thread) → plan a cable-gland / grommet duct-wall mount; or buy a "DS18B20 G1/2″ thread" SKU.

**NTC fallback:** if only one channel is ever needed and cost is critical, a 10 kΩ 1 % NTC into an ADC pin (10 kΩ bias + RC filter) still works and matches the old `ANALOG_IN` model — but it loses the single-bus multi-probe advantage.

---

## 5. Open items — need your input before finalizing

1. **Number of probes / lead length** — two (incoming + outgoing), each on its own single-drop 1-Wire line (§4.1). How long is each lead, and does the node PCB sit right at the duct wall or is it a longer run? (1-Wire tolerates several metres; size the per-line pull-up down toward ~2.2 kΩ if leads get long.)
2. **Value encoding on the bus** — the protocol spec leaves `DATA` interpretation "per `OPERATION`/`CHANNEL` convention, not enforced by the frame" (protocol spec §3). Needs a concrete decision here: e.g. `int16` in units of 0.1 °C, signed to allow sub-zero outdoor readings. (The DS18B20 native format is `int16` in 1/16 °C — a clean fit.)
3. **Update/report rate** — how often does a temperature reading need to change the bus state? (Duct air temperature changes slowly compared to, say, a digital input — the existing debounce-on-change pattern may need a minimum report interval added on top, not just change-detection, so `MainController` doesn't conclude the node is dead during a long stretch of unchanged readings — though note the heartbeat/poll cycle already covers liveness independent of value changes.)
4. **Enclosure ingress rating** — the near-outdoor-unit location may need a sealed/IP-rated enclosure; not a hardware-spec concern but flag it for the mechanical design.
