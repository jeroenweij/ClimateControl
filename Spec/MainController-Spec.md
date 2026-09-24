# MainController — Design Spec

**Companion docs:** `RS485-Node-Protocol-Spec-STM32G030.md` (wire protocol this module implements as master), `Node-Bus-Hardware-Design-Spec.md` §6 (shared node core schematic this board reuses), `Node-Bus-Power-Path-Spec.md` §4 (both-ends feed decision), `Software-Architecture-Spec.md` (module map)

---

## 1. Role

`MainController` is the RS485 bus master for the main node bus, running the same STM32G0-family MCU as the slave nodes (`ControllerNode`, `TemperatureNode`). It owns node ID `0` (reserved per protocol spec §6) and implements the `NodeMaster` side of `NodeLib`:

- Discovery: broadcasts `Discover`, collects staggered `Announce` replies (each carrying the node's module type + 96-bit UID) and builds the `UID→NodeId` roster (protocol spec §6, `Node-Message-Model-Spec.md` §4).
- Poll cycle: round-robins active nodes with `Poll` → node dumps its queued `Report`s → `Done` → poll next (protocol spec §6, `PollNextNode`/`ActiveNodeCount`/`activeNodes[]`).
- Heartbeat: resets a timer each full poll round; declares `ConnectionLost()` on lapse.

This is `NodeMaster`, re-targeted to the v2 variable-length/CRC framing.

---

## 2. What MainController does with the data it collects

Setpoint control is a local loop between each `ControllerNode` and its own `Thermostat` (`ControllerNode-Thermostat-Link-Spec.md`), not routed through `MainController`. `MainController`'s role is supervisory — aggregate temperatures, expose system state, detect faults — plus one piece of fleet-wide arbitration: it divides a shared airflow "budget" across all online `ControllerNode`s (`Damper-Budget-Spec.md` §5, `BudgetAllocator`), sent as a per-node `DamperBudget` ceiling. This is not closed-loop room control — it never sees or reacts to a single room's temperature error the way `ControllerNode`'s own loop does, it only narrows what that loop is allowed to do.

---

## 3. Power input & bus injection

`MainController` is the near-end **48 V injection point** for the shared bus, and a second PSU feeds the far end. The board reuses the `Node-Bus-Hardware-Design-Spec.md` §6 node core (MCU, transceiver, LEDs, buttons, local 48→5→3.3 V chain); this section is only the extra injection front-end, which is **DNP on `TemperatureNode` builds** of the same board.

**Input connector — P1:** 3-position 5.08 mm screw terminal (`WJ500V-5.08-03P-14-00A`). GND / +48 V / GND (double GND for return current). Silkscreen the polarity clearly.

**Protection chain: P1 → fuse → TVS → +48 V rail → bus RJ45s.**

| Function | Part | Notes |
|---|---|---|
| Fuse | 5×20 mm **ceramic** (sand-filled), **time-lag (T)**, DC rating ≥ 63 V, in a PCB holder (XFCN PTF-77, LCSC `C717030`) | Field-replaceable, hand-inserted (not part of the PCB assembly). |
| Surge / reverse-polarity TVS — D4 | **SMCJ58A**, 1500 W, SMC, unidirectional (LCSC `C3012585`) | Cathode → +48 V rail, anode → GND. Clamps cable/PSU transients. On a reverse-wire at P1 it forward-conducts and blows the fuse — D4 is sacrificial then. Assumes a supply regulated near 48 V; if the bus can reach the 57 V top of the range use SMCJ60A/64A. |

Every node incl. the master already has its own series Schottky (SS26A) guarding its regulator + electrolytic, so there is no separate series reverse-polarity Schottky on the raw +48 V rail here — D4 + fuse handle a reverse-wire non-catastrophically without one.

**Feed topology: two PSUs**, one at each end of the bus. Master fuse carries ~0.7 A normal / **~3.2 A** worst-case (all 20 servos stalled at once, split across both injection points) — **T4 A** ceramic, margin ~1.25×. The worst-case aggregate (20 nodes × ~2.7A servo stall current, `Node-Bus-Hardware-Design-Spec.md` §4) is ~6.4A at 48V split across both ends; whether simultaneous full-stall across all 20 nodes is realistic for this application is open (`Node-Bus-Hardware-Design-Spec.md` §7) — a T-type fuse rides over a few-second synchronised-homing move regardless.

The far-end feed is either a second *TemperatureNode-variant* Main board (the only other board variant carrying the injection front-end) or a bare external 48 V supply wired directly into that end's RJ45 power pins — the injection front-end is not populated on a `ControllerNode` board.

Order fuse (T4 A ceramic 5×20, both-ends feed): ESKA 522.523 — <https://www.amazon.nl/G-veiligheidsinzet-zekering-5x20mm-522-523-drager/dp/B01MV3477I>

---

## 4. Internet connectivity — NINA-W152

u-blox **NINA-W152** (Wi-Fi b/g/n + BT, integrated PIFA antenna) on **USART2**, running the pre-flashed **u-connectXpress AT firmware** — TCP/IP + TLS run on the module, the STM32 only drives a UART at 115200 8N1. The outward interface **relays `NodeLib` v2 frames verbatim over one plaintext LAN TCP socket to a server** that decodes, stores to SQLite, and serves an SPA over HTTP + WebSocket — full design in `MainController-Server-Link-Spec.md`. No local display/buttons.

**Minimal connections:**

| NINA pin | To | Notes |
|---|---|---|
| VCC (10) + VCC_IO (9) | `+3v3 P` rail — dedicated RT9080-33GJ5 LDO off the 5 V buck | see `Node-Bus-Power-Path-Spec.md` §4.1. ~120 mA avg / ~350 mA peak. Bulk: 22 µF at the LDO + ≥10 µF right at the NINA VCC pins (NINA is in the opposite board corner) + 100 nF. RT9080 populated on both Main-board build variants. |
| GND + centre pad | solid ground pour | |
| RESET_N (19) | STM32 `PA6` (net RESET_NINA), **open-drain**, active low | module has 100 kΩ + 10 nF internal; drive low ≥50 µs, release to run. Never push-pull — `Hal::Gpio::Mode::OpenDrain` (`Write(false)` = drive low, `Write(true)` = release to Hi-Z). Add a 10 kΩ pull-up to `+3v3 P` + a test point. |
| UART_RXD (23) | STM32 `PA2` (USART2_TX, AF1) | |
| UART_TXD (22) | STM32 `PA3` (USART2_RX, AF1) | |
| UART_CTS (21) | STM32 `PA1` (USART2_RTS, AF1) | 4-wire HW flow control, on by default in u-connectXpress: the module will not transmit at all until this line is driven low by the STM32 (push-pull output, held low) — there is nothing else on the link to assert it. |
| UART_RTS (20) | STM32 `PA0` (USART2_CTS, AF1) | freed by moving RESET_NODES to `PB9` |
| boot pins 27/32/36 | leave unconnected | internally strapped; pin 36 must not be pulled low |
| SWITCH_1 (7), SWITCH_2 (18) | 2 test pads each (or 0 Ω-DNP to GND) | SWITCH_1 low at boot = restore UART defaults; SWITCH_1+2 low = enter serial bootloader. Recovery path if FW/baud is lost |
| UART_TXD/RXD | header H1 (shared with USART2) | firmware update via AT or bootloader; hold the STM32 in reset (NRST on the Tag-Connect) to drive H1 from a PC adapter |
| ANT (13) | leave open (or to GND) | W152 = internal antenna. Module in a board corner, antenna edge to the board edge, no copper on any layer under the antenna keep-out, ≥10 mm from P1 / RJ45 / electrolytics / the buck node, plastic enclosure only |

**Consequence:** both USARTs are committed (USART1 = bus, USART2 = NINA) → no hardware debug console on this board (LPUART1 also lands on PA2/PA3 on TSSOP20). The log is pushed to the server over the uplink instead (`MainController-Server-Link-Spec.md` §5.1), with an optional SEGGER RTT sink for the bench.

Firmware baseline is u-connectXpress **6.4.1-001**; factory/sample units may ship on much older firmware and should be updated via s-center before deployment. Wi-Fi station join and a TCP connection to the server's uplink port have both been verified end-to-end against real hardware — command sequence in `MainController-Server-Link-Spec.md` §3.

---

## 5. Persistence

`MainController` needs no persistence across power cycles. Setpoints live locally on each `Thermostat`/`ControllerNode` pair (`ControllerNode-Thermostat-Link-Spec.md`), the node roster rebuilds itself every boot via `Discover`/`Announce` (protocol spec §6), and `BudgetAllocator` recomputes fresh from bus traffic with no saved state across a reset (`Damper-Budget-Spec.md` §7). No internal flash or external EEPROM/flash chip is needed for this.
