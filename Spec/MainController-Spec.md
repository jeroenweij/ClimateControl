# MainController — Design Spec

**Status:** Draft — bus-master role confirmed 2026-09-06; power input defined (§3); internet connectivity = NINA-W152 (§5, 2026-09-08); what it does with the data still open (see §4)
**Companion docs:** `RS485-Node-Protocol-Spec-STM32G030.md` (wire protocol this module implements as master), `Node-Bus-Hardware-Design-Spec.md` §6 (shared node core schematic this board reuses), `Node-Bus-Power-Path-Spec.md` §4 (both-ends feed decision), `Software-Architecture-Spec.md` (module map)

---

## 1. Confirmed role

`MainController` is the RS485 bus master for the main node bus, running the same STM32G0-family MCU as the slave nodes (`ControllerNode`, `TemperatureNode`). It owns node ID `0` (reserved per protocol spec §6) and implements the `NodeMaster` side of `NodeLib`:

- Discovery: broadcasts `Discover`, collects staggered `Announce` replies (each carrying the node's module type + 96-bit UID) and builds the `UID→NodeId` roster (protocol spec §6, `Node-Message-Model-Spec.md` §4).
- Poll cycle: round-robins active nodes with `Poll` → node dumps its queued `Report`s → `Done` → poll next (protocol spec §6, `PollNextNode`/`ActiveNodeCount`/`activeNodes[]`).
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

1. **User/network interface.** *Partially resolved 2026-09-08:* internet connectivity is a **u-blox NINA-W152** (Wi-Fi + BT, internal antenna) on USART2, running u-connectXpress AT firmware — the TCP/IP + TLS stack lives on the module, so the STM32 only needs a UART. Minimal connections and the power/flow-control decisions are in §5 (below). Still open: what the MainController *does* with that connectivity (MQTT? local REST? Home Assistant?) and whether there is also a local display/buttons.
2. **Control loop ownership.** Per §2 above: does `MainController` make any climate-control decisions itself, or is it purely a bus master + data logger while each `ControllerNode`/`Thermostat` pair handles its own room's control loop locally?
3. **Feed topology (§3):** two PSUs (one per bus end) or one PSU at the master feeding both ends via a return cable? Decides the master fuse rating (T4 A vs T6.3 A) and the +48 V copper sizing.
4. **Persistence.** Does `MainController` need to remember anything across power cycles (schedules, setpoints, node roster) — and if so, where (internal flash, external EEPROM/flash chip)?
5. **Same MCU as slave nodes — resolved 2026-09-08: STM32G031F8P6** (64 KB flash / 8 KB SRAM, drop-in for the STM32G030F6P6TR). The +32 KB flash covers a bus-resident DFU bootloader plus the NINA AT-driver; RAM is unchanged at 8 KB, so the §7 memory discipline in `RS485-Node-Protocol-Spec-STM32G030.md` still applies. Footprint also fits STM32G031F6P6 (32 KB) as a cost-down fallback for the slave nodes.

---

## 5. Internet connectivity — NINA-W152 (decided 2026-09-08)

u-blox **NINA-W152** (Wi-Fi b/g/n + BT, integrated PIFA antenna) on **USART2**, running the pre-flashed **u-connectXpress AT firmware** — TCP/IP + TLS run on the module, the STM32 only drives a UART at 115200 8N1.

**Minimal connections:**

| NINA pin | To | Notes |
|---|---|---|
| VCC (10) + VCC_IO (9) | `+3v3 P` rail — dedicated RT9080-33GJ5 LDO off the 5 V buck | see `Node-Bus-Power-Path-Spec.md` §4.1. ~120 mA avg / ~350 mA peak. **Bulk: 22 µF at the LDO + ≥10 µF right at the NINA VCC pins** (NINA is in the opposite board corner) + 100 nF. RT9080 populated on both Main-board build variants. |
| GND + centre pad | solid ground pour | |
| RESET_N (19) | STM32 `PA6` (net RESET_NINA), **open-drain**, active low | module has 100 kΩ + 10 nF internal; drive low ≥50 µs, release to run. Never push-pull. Add a 10 kΩ pull-up to `+3v3 P` + a test point (defensive; `Hal::Gpio::Mode` needs an open-drain option added). |
| UART_RXD (23) | STM32 `PA2` (USART2_TX, AF1) | |
| UART_TXD (22) | STM32 `PA3` (USART2_RX, AF1) | |
| UART_CTS (21) | STM32 `PA1` (USART2_RTS, AF1) | 4-wire HW flow control (on by default in u-connectXpress) |
| UART_RTS (20) | STM32 `PA0` (USART2_CTS, AF1) | freed by moving RESET_NODES to `PB9` |
| boot pins 27/32/36 | leave unconnected | internally strapped; pin 36 must not be pulled low |
| SWITCH_1 (7), SWITCH_2 (18) | 2 test pads each (or 0 Ω-DNP to GND) | SWITCH_1 low at boot = restore UART defaults; SWITCH_1+2 low = enter serial bootloader. Recovery path if FW/baud is lost |
| UART_TXD/RXD | header H1 (shared with USART2) | firmware update via AT or bootloader; hold the STM32 in reset (NRST on the Tag-Connect) to drive H1 from a PC adapter |
| ANT (13) | leave open (or to GND) | W152 = internal antenna. Module in a board corner, antenna edge to the board edge, **no copper on any layer under the antenna keep-out**, ≥10 mm from P1 / RJ45 / electrolytics / the buck node, plastic enclosure only |

**Consequence:** both USARTs are now committed (USART1 = bus, USART2 = NINA) → no hardware debug console on this board (LPUART1 also lands on PA2/PA3 on TSSOP20). Bit-bang `Tools::Logger` on PA4/PA5/PC15 or accept no console.

**Board-rev review (2026-09-08, `Hardware/Main/*_PCB1_1_2026-09-08`):** pin map and NINA/RT9080 wiring verified correct. Open before fab: (1) confirm the NINA antenna keep-out and clearances above; (2) move ≥10 µF of the NINA bulk to the module's VCC pins; (3) add the SWITCH_1/2 and RESET_N pads above; (4) commit a schematic PDF alongside the layout; (5) confirm the orderable NINA-W152 variant (BOM shows `-04B`; datasheet current production is `-06B`).
