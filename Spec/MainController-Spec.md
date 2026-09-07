# MainController — Design Spec

**Status:** Draft — bus-master role confirmed 2026-09-06; power input defined (§3); outward-facing responsibilities are open (see §4)
**Companion docs:** `RS485-Node-Protocol-Spec-STM32G030.md` (wire protocol this module implements as master), `Node-Bus-Hardware-Design-Spec.md` §6 (shared node core schematic this board reuses), `Node-Bus-Power-Path-Spec.md` §4 (both-ends feed decision), `Software-Architecture-Spec.md` (module map)

---

## 1. Confirmed role

`MainController` is the RS485 bus master for the main node bus, running the same STM32G0-family MCU as the slave nodes (`ControllerNode`, `TemperatureNode`). It owns node ID `0` (reserved per protocol spec §6) and implements the `NodeMaster` side of `NodeLib`:

- Discovery: broadcasts `DETECTNODES`, collects staggered `HELLOWORLD` replies (protocol spec §6).
- Poll cycle: round-robins active nodes with `SENDQ` → node dumps queue → `ENDOFQ` → poll next (protocol spec §6, `PollNextNode`/`ActiveNodeCount`/`activeNodes[]`).
- Heartbeat: resets a timer each full poll round; declares `ConnectionLost()` on lapse.

This is a direct port of `NodeMaster` from `~/git/node/Software/lib/NodeLib/NodeMaster.{h,cpp}`, re-targeted to the v2 variable-length/CRC framing instead of the fixed-size AVR frame.

---

## 2. What MainController does with the data it collects

Once a `VALUE` message arrives from a `ControllerNode` or `TemperatureNode`, something has to decide what to do with it — e.g. compare `TemperatureNode` readings against a `Thermostat` setpoint and command the relevant `ControllerNode`'s damper. **Note:** per `ControllerNode-Thermostat-Link-Spec.md`, setpoint control is currently expected to be a local loop between each `ControllerNode` and its own `Thermostat`, not routed through `MainController`. If that's correct, `MainController`'s role may be closer to *supervisory/logging* (aggregate temperatures, expose system state, detect faults) than closed-loop control. **This needs your confirmation** — it changes what `MainController` needs to do beyond running the bus.

---

## 3. Power input & bus injection

`MainController` is the near-end **48 V injection point** for the shared bus. Per `Node-Bus-Power-Path-Spec.md` §4 the bus is fed at **both ends** — the far end needs its own connection (see topology decision below). The board reuses the `Node-Bus-Hardware-Design-Spec.md` §6 node core (MCU, transceiver, LEDs, buttons, local 48→5→3.3 V chain); this section is only the extra injection front-end, which is **DNP on `TemperatureNode` builds** of the same board.

**Input connector — P1:** 3-position 5.08 mm screw terminal (`WJ500V-5.08-03P-14-00A`). GND / +48 V / GND (double GND for return current; or make one outer pin a chassis-earth for the RJ45 shield). **Silkscreen the polarity clearly.**

**Protection chain: P1 → fuse → TVS → +48 V rail → bus RJ45s.**

| Function | Part | Notes |
|---|---|---|
| Fuse | 5×20 mm **ceramic** (sand-filled), **time-lag (T)**, DC rating ≥ 63 V, in a PCB holder (XFCN PTF-77, LCSC `C717030`) | Field-replaceable. Value per feed topology below. Ceramic not glass — glass has poor DC breaking. |
| Surge / reverse-polarity TVS — D4 | **SMCJ58A**, 1500 W, SMC, unidirectional (LCSC `C3012585`) | Cathode → +48 V rail, anode → GND. Clamps cable/PSU transients. On a reverse-wire at P1 it forward-conducts and blows the fuse — D4 is sacrificial then (~€0.10). Assumes a supply regulated near 48 V; if the bus can reach the 57 V top of the range use SMCJ60A/64A. |
| ~~Series reverse-polarity Schottky~~ | **removed 2026-09-07** (was MBR10100) | Redundant: every node incl. the master already has its own series Schottky (SS26A) guarding its regulator + electrolytic; nothing on the raw +48 V rail is polarity-sensitive; and D4 + fuse handle a reverse-wire non-catastrophically. It was also the hottest part on the board and cost a 0.5 V drop. |

**Feed topology — decides the fuse rating (still to confirm):**

| Topology | Master fuse carries | Fuse |
|---|---|---|
| **Two PSUs** — one at each end of the bus | ~0.7 A normal / **~2.4–2.7 A** worst-case (all 20 servos stalled at once) | **T4 A** |
| **One PSU at the master**, feeding the far end via a return cable | ~1.4 A normal / **~4.7 A** worst-case | **T6.3 A** — and size the master's +48 V copper for ~6 A |

Full-system load is not the constraint the intuition suggests: 20 nodes + 20 servos + 20 thermostat displays, *all stalled simultaneously*, is ~4.7 A total at 48 V (that's the point of the 48 V rail — `Node-Bus-Power-Path-Spec.md` §2). Whether simultaneous full-stall is even realistic is still open (`Node-Bus-Power-Path-Spec.md` §7 item 2); a T-type fuse rides over a few-second synchronised-homing move regardless. **Default: two PSUs + T4 A ceramic.**

Order fuse (T4 A ceramic 5×20, both-ends feed): ESKA 522.523 — <https://www.amazon.nl/G-veiligheidsinzet-zekering-5x20mm-522-523-drager/dp/B01MV3477I>

---

## 4. Open items — need your input before finalizing

1. **User/network interface.** Nothing in the Specs so far describes how a person or another system observes or configures `MainController` — no display, buttons, Ethernet/Wi-Fi, serial console, or home-automation integration (e.g. MQTT, Home Assistant) has been mentioned. If there is one, it drives peripheral choices (does the STM32G030's 32 KB flash / 8 KB SRAM even fit a network stack, or does this need a bigger part / a companion SBC?).
2. **Control loop ownership.** Per §2 above: does `MainController` make any climate-control decisions itself, or is it purely a bus master + data logger while each `ControllerNode`/`Thermostat` pair handles its own room's control loop locally?
3. **Feed topology (§3):** two PSUs (one per bus end) or one PSU at the master feeding both ends via a return cable? Decides the master fuse rating (T4 A vs T6.3 A) and the +48 V copper sizing.
4. **Persistence.** Does `MainController` need to remember anything across power cycles (schedules, setpoints, node roster) — and if so, where (internal flash, external EEPROM/flash chip)?
5. **Same MCU as slave nodes, confirmed** — but does `MainController` need more flash/RAM than the 32 KB/8 KB STM32G030F6P6TR once its actual responsibilities (§1–4) are known? Flagging now since the answer to #1 will likely force this decision.
