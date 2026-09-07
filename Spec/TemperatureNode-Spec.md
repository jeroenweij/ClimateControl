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
- Shares the same 48V-in/RJ45 power and RS485 physical layer as every other node on the bus (`Node-Bus-Hardware-Design-Spec.md` §2–§6) — no separate power design needed unless the outdoor/near-duct location demands different ingress protection on the enclosure (not a hardware-spec concern, but worth flagging for the mechanical/enclosure design).

---

## 4. Sensor — DS18B20 on a 1-Wire probe lead

Chosen 2026-09-07: **DS18B20**, 1-Wire, mounted in a stainless probe on a short lead.

**Why DS18B20 over an NTC:**
- **One bus, both channels.** 1-Wire is a bus — the "incoming" and "outgoing" probes sit on the *same three wires* (VDD/GND/DQ) and are read individually by their 64-bit ROM ID. An NTC would need a separate ADC channel + bias resistor + calibration table per probe.
- No ADC calibration / Steinhart-Hart table; digital reading is immune to lead-length and analog noise.
- −55…+125 °C, ±0.5 °C — covers hot supply air and sub-zero return/outdoor air with margin.
- A pre-made **waterproof stainless-tube probe** (SS tube + ~0.5 m lead) handles condensation on the cooling side and gives a rugged in-airflow probe. e.g. LCSC `C843306`, or a bare DS18B20 (`C376006`, UMW, TO-92) potted on a pigtail.

### 4.1 On-board front-end (TemperatureNode only — not on the shared "Base" sheet)

The probes are off-board cable assemblies; the PCB adds a connector per probe, one pull-up, and light protection.

| Ref | Part | Purpose |
|---|---|---|
| J_T1, J_T2 | 3-pos 3.5 mm screw terminal (e.g. KF128-3.5-3P), one per probe | land the bare probe leads with a screwdriver |
| R_1W | 4.7 kΩ, 0402 | DQ → 3V3 pull-up — **one**, for the whole bus |
| C_1W | 100 nF, 0402 | probe VDD → GND, at the connectors |
| R_ser *(optional)* | 100 Ω, 0402 | in the DQ line between the MCU pin and the connector node — MCU-pin protection |
| D_1W *(optional)* | single-line ESD diode to GND (e.g. PESD3V3L1BA) | DQ leaves the enclosure |

```
 +3V3 ──┬── R_1W (4.7k) ──┐
        │                 │
        │        ┌────────●── ONEWIRE_BUS ──┬── J_T1 pin 2 (DATA)
 PA0 ──[R_ser]───┘                          ├── J_T2 pin 2 (DATA)
        │                                   └── D_1W ─┴─ GND   (optional)
 C_1W (100n) ─┴─ GND

 +3V3 ── J_T1 pin 1,  J_T2 pin 1        (VDD — DS18B20 runs at 3.3 V, NOT 5 V)
 GND  ── J_T1 pin 3,  J_T2 pin 3
```

- Both connectors sit **in parallel on one bus**; the probes are told apart by ROM ID.
- Pull-up + ESD diode go on the connector side of R_ser; R_ser between there and the MCU pin.
- **DQ pin: PA0** (pin 7) — free on the TemperatureNode (no servo). USART2 (PA2/PA3) stays for the debug header, so this is a plain GPIO bit-bang, not the USART-1-Wire trick.
- Twisted/shielded lead not needed at these lengths; keep the pull-up at the board end.

**Firmware:** 1-Wire bit-bang on PA0, pin as open-drain (drive low / release, R_1W pulls high), timing-critical slots with interrupts briefly masked — ~1–2 KB. Enumerate ROM IDs at startup; map them to "incoming"/"outgoing" by stored config or a one-time labelled reading. DS18B20's native `int16` in 1/16 °C is a clean bus value format.

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

1. **Number of probes / lead length** — confirmed at least two (incoming + outgoing) on one 1-Wire bus. How long is each lead, and does the node PCB sit right at the duct wall or is it a longer run? (1-Wire tolerates several metres; just size the pull-up down toward ~2.2 kΩ if leads get long.)
2. **Value encoding on the bus** — the protocol spec leaves `DATA` interpretation "per `OPERATION`/`CHANNEL` convention, not enforced by the frame" (protocol spec §3). Needs a concrete decision here: e.g. `int16` in units of 0.1 °C, signed to allow sub-zero outdoor readings. (The DS18B20 native format is `int16` in 1/16 °C — a clean fit.)
3. **Update/report rate** — how often does a temperature reading need to change the bus state? (Duct air temperature changes slowly compared to, say, a digital input — the existing debounce-on-change pattern may need a minimum report interval added on top, not just change-detection, so `MainController` doesn't conclude the node is dead during a long stretch of unchanged readings — though note the heartbeat/poll cycle already covers liveness independent of value changes.)
4. **Enclosure ingress rating** — the near-outdoor-unit location may need a sealed/IP-rated enclosure; not a hardware-spec concern but flag it for the mechanical design.
