# TemperatureNode — Design Spec

**Status:** Draft — role confirmed 2026-09-06, sensor/hardware choices open (see §4)
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

## 4. Open items — need your input before finalizing

1. **Sensor part.** No sensor has been chosen. Options to pick between (or you may already have one in mind):
   - Digital sensor (e.g. I2C/1-Wire like an SHT3x, DS18B20) — better accuracy/noise immunity over a short board trace to the probe, simpler firmware (no ADC calibration), but adds a bus/driver dependency.
   - Analog (NTC thermistor into the STM32's ADC) — cheapest, matches the old `ANALOG_IN` channel model almost exactly, but needs a calibration/lookup table and is more sensitive to wiring noise on a longer probe lead.
2. **Probe/lead length** — how far is the sensor from the node PCB itself (inline in the duct vs. node mounted right at the duct wall)? Affects sensor choice above and whether shielded/twisted leads are needed.
3. **Value encoding on the bus** — the protocol spec leaves `DATA` interpretation "per `OPERATION`/`CHANNEL` convention, not enforced by the frame" (protocol spec §3). Needs a concrete decision here: e.g. `int16` in units of 0.1 °C, signed to allow sub-zero outdoor readings.
4. **Update/report rate** — how often does a temperature reading need to change the bus state? (Duct air temperature changes slowly compared to, say, a digital input — the existing debounce-on-change pattern may need a minimum report interval added on top, not just change-detection, so `MainController` doesn't conclude the node is dead during a long stretch of unchanged readings — though note the heartbeat/poll cycle already covers liveness independent of value changes.)
