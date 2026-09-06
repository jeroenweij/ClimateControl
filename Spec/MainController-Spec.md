# MainController — Design Spec

**Status:** Draft — bus-master role confirmed 2026-09-06; outward-facing responsibilities are open (see §3)
**Companion docs:** `RS485-Node-Protocol-Spec-STM32G030.md` (wire protocol this module implements as master), `Software-Architecture-Spec.md` (module map)

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

## 3. Open items — need your input before finalizing

1. **User/network interface.** Nothing in the Specs so far describes how a person or another system observes or configures `MainController` — no display, buttons, Ethernet/Wi-Fi, serial console, or home-automation integration (e.g. MQTT, Home Assistant) has been mentioned. If there is one, it drives peripheral choices (does the STM32G030's 32 KB flash / 8 KB SRAM even fit a network stack, or does this need a bigger part / a companion SBC?).
2. **Control loop ownership.** Per §2 above: does `MainController` make any climate-control decisions itself, or is it purely a bus master + data logger while each `ControllerNode`/`Thermostat` pair handles its own room's control loop locally?
3. **Persistence.** Does `MainController` need to remember anything across power cycles (schedules, setpoints, node roster) — and if so, where (internal flash, external EEPROM/flash chip)?
4. **Same MCU as slave nodes, confirmed** — but does `MainController` need more flash/RAM than the 32 KB/8 KB STM32G030F6P6TR once its actual responsibilities (§1–3) are known? Flagging now since the answer to #1 will likely force this decision.
