# ControllerNode ↔ Thermostat Link — Design Spec

**Status:** Draft — topology and protocol-reuse approach (§2) confirmed 2026-09-06, physical-layer details still open (see §4)
**Companion docs:** `RS485-Node-Protocol-Spec-STM32G030.md` (main bus protocol this link is *not* using wholesale), `Software-Architecture-Spec.md` (module map)

---

## 1. Confirmed topology

Each `ControllerNode` (damper actuator, main-bus slave) has exactly one `Thermostat` (room UI/setpoint) paired to it. That pair talks over a **separate, dedicated link — not the main RS485 bus**:

```
MainController ──(main RS485 bus, many nodes)── ControllerNode ──(dedicated 2-endpoint link)── Thermostat
                                                  ControllerNode ──(dedicated 2-endpoint link)── Thermostat
                                                  ControllerNode ──(dedicated 2-endpoint link)── Thermostat
                                                       ...                                          ...
```

- `ControllerNode` is on the main bus as a normal `NodeLib` slave (node ID `1..N`, per the protocol spec).
- `Thermostat` is **not** addressable on the main bus at all — it only ever talks to its own `ControllerNode`.
- Because the link has exactly two fixed endpoints, the main protocol's round-robin arbitration (`DETECTNODES`/`HELLOWORLD`/`SENDQ`/`ENDOFQ`, protocol spec §6) exists to let one master poll an unknown number of slaves fairly — that problem doesn't exist here. A full port of the main-bus state machine would be solving a problem this link doesn't have.

---

## 2. What carries over from NodeLib vs. what doesn't (confirmed)

| Protocol-spec concept | Carries over? | Why |
|---|---|---|
| `Id`/`Message` framing, CRC16, sync/resync (protocol spec §3–§5) | **Yes** | Still useful for a clean, corruption-detected frame even on a 2-endpoint link — no reason to invent a different frame format just because there's no arbitration. |
| `ChannelId`/`Operation` enums | **Partially** | The *concept* (typed channel + operation) still fits (e.g. `SETPOINT`, `ROOM_TEMP`, `MODE`), but the specific values (`DIGITAL_1`, `SERVO_1`, `DETECTNODES`, ...) are main-bus-specific and don't apply here. |
| `DETECTNODES`/`HELLOWORLD` discovery | **No** | Fixed 1:1 pairing — nothing to discover. |
| `SENDQ`/`ENDOFQ` round-robin polling | **No** | No arbitration needed between exactly two endpoints; a simpler request/response or periodic-push exchange is sufficient. |
| Heartbeat/`ConnectionLost()` | **Yes, conceptually** | Still want to detect a dead/disconnected `Thermostat` (or vice versa) — just doesn't need the full poll-cycle machinery to drive it, a simple periodic ping/ack works. |

**Confirmed:** don't reuse `NodeLib::Node`/`NodeMaster` as-is for this link. Instead, factor the reusable parts (`Id`/`Message`/CRC framing) into something shared — a candidate for `Software/Lib/NodeLib` itself, split so the framing layer doesn't drag in the round-robin master/slave state machine — and write a much smaller point-to-point exchange (simple request/response or periodic push + ping/ack for liveness) specifically for this link, rather than adapting `NodeMaster`'s arbitration to a degenerate 2-node case.

---

## 3. Physical layer

You noted this is "possibly also a RS485 bus with just 2 endpoints" — RS485's differential signaling is a reasonable choice here independent of the arbitration question, since it's still a wired link that may run a non-trivial distance from a duct-mounted `ControllerNode` to a wall-mounted room thermostat, and differential signaling resists noise better than single-ended UART over that distance. But since there are only ever two endpoints, it doesn't need the transceiver DE/RE half-duplex switching that the main bus needs for multi-drop arbitration — it could run full-duplex (two independent differential pairs, or even a simple UART if the run is short enough) instead of half-duplex RS485. **This needs your input** — see open items below.

---

## 4. Open items — need your input before finalizing

1. **Physical link:** half-duplex RS485 (2-wire, matching the main bus parts/BOM for consistency), full-duplex RS485/RS422-style (4-wire), or plain UART (if cable runs are always short, e.g. within one room)?
2. **Cable/connector:** does this link ride on the same RJ45/Ethernet-cable infrastructure as the main bus (`Node-Bus-Hardware-Design-Spec.md` §3, spare conductors?) or a separate cable run entirely? The main-bus RJ45 pinout in that spec is already fully allocated (RS485 pair, 48V pair, GND pair, RESET+ENABLE pair) — there's no spare pair for a second differential link on the same cable.
3. **Thermostat power:** does `Thermostat` draw power from `ControllerNode` over this same link/cable, or does it have its own local supply (e.g. mains-adjacent wall power, batteries)? Affects both this spec and a possible future `Thermostat` power-path spec.
4. **Thermostat UI/hardware:** display type, buttons/dial for setpoint, does it have its own room-temperature sensor onboard (implied by "control that room"), MCU (same STM32G0 family assumed per your answer, but flagging for confirmation).
5. **Control loop location:** confirm — does `ControllerNode` itself run the room's thermostat control loop (compare `Thermostat`'s setpoint/room-temp against damper position and act locally), with `MainController` only seeing the results over the main bus? This is the assumption `MainController-Spec.md` §2 is currently built on.
