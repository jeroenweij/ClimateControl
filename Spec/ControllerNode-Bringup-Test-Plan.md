# ControllerNode — Hardware Bring-up Test Plan
### What to check, in order, when the first real ControllerNode boards (servo, INA180, Thermostat link) arrive

**Companion docs:** `Node-Bus-Hardware-Design-Spec.md` (servo, linkage, pin map), `Node-Bus-Power-Path-Spec.md` §3.1 (servo rail gating, stall detection), `Damper-Budget-Spec.md` (room loop and its tunable constants), `ControllerNode-Thermostat-Link-Spec.md` (link + Thermostat), `Node-Flash-Layout-and-Bootloader-Spec.md` (flash, provisioning, OTA).

Everything below is implemented in firmware and covered by host unit tests, but has only ever run on MainController PCBs with the ControllerNode build — no servo, no current-sense amp, no link transceiver. Each step lists what to do, what should happen, and what to change if it doesn't. Work top to bottom: later steps assume the earlier ones passed.

---

## 1. Tools and how-to

- **Scope** on `PA6` (servo signal, pin 13), the switched servo rail, and `PA0` (`SENSE`, INA180 output). A second channel on the 5 V and 3.3 V rails for §5.
- **Command a move** through the bench server (`MainController-Server-Link-Spec.md`) or the Overrides page. A target switches the damper to `Manual` by itself, so the room loop won't move it back:
  ```
  curl -X POST http://<server>:8090/api/commands -d '{"node":<id>,"endpoint":"DamperTarget","value":50}'
  ```
  `DamperMode` `2` returns it to `Auto` (and drops the held target).
- **Watch it on the Status page:** the damper shows `actual → target` until a move lands, and the **Fault** column shows an active stall in red, then "last: Damper stalled <time>" after it clears.
- **Read the stall-sense ADC while a move runs:** J-Link `mem32 0x40012440 1` reads `ADC1_DR`, the last conversion. J-Link reads memory without halting, and `Damper::Loop()` samples every pass while the servo is powered.
- **Node log:** `GET /api/nodes/<id>/log` drains the node's DiagLog ring (move, stall and park messages from `Damper`).

## 2. Board power-up (no servo connected)

| # | Do | Expect | If not |
|---|---|---|---|
| 2.1 | Power the board with the MCU still blank | Servo rail **0 V** (P-FET off with the gate pulled up) | Load-switch/level-shift circuit fault — fix before any firmware |
| 2.2 | `make -C Software flash-full MODULE=controllerNode`, then `Software/tools/provision.py --node-id <n> --module controller` | Node joins the bus; server shows it as ControllerNode; after 10 s the boot counter is 0 (J-Link `mem32 0x4000B104 1`) | See J-Link notes in `tools/README.md` |
| 2.3 | Hold the node in reset (J-Link `r`, don't `g`) | Servo rail 0 V, `PA6` not driven | — |

## 3. Servo signal (servo disconnected)

| # | Do | Expect | If not |
|---|---|---|---|
| 3.1 | Idle | `PA6` low, rail off | — |
| 3.2 | `DamperTarget` 0 / 50 / 100 | 50 Hz (20.0 ms period), high time **500 / 1500 / 2500 µs**, within ~1 % (HSI16 tolerance) | Period or width off by a constant factor → timer clock / prescaler (`Hal::Pwm::Init()`) |
| 3.3 | Same, watch rail and signal together | Pulse present from the moment the rail switches on, for ~1.5 s (`moveSettleMs`); then rail off and `PA6` low | — |
| 3.4 | Change target while a move is running | No short or stretched pulse at the change (the new width lands on the next period) | — |

## 4. Servo alone (connected, no linkage)

| # | Do | Expect | If not |
|---|---|---|---|
| 4.1 | `DamperTarget` 0 / 50 / 100 | Horn at 0° / 90° / 180°, direction consistent between units | — |
| 4.2 | Hold at 0 and at 100 while powered | No buzzing or humming against the servo's internal stop | Narrow `closedPulseUs` / `openPulseUs` in `Damper.h` (e.g. 550 / 2450) — the same for every unit |
| 4.3 | Full 0 → 100 move, time it on the scope (rail current) | Settles well inside 1.5 s (DS3225 ~0.4 s for 180° at 5 V) | Slower → raise `moveSettleMs` in `Damper.h` |
| 4.4 | After the rail switches off, push the horn gently | Holds position (gearing holds it unpowered) | — |

## 5. Current sense and stall detection

| # | Do | Expect | If not |
|---|---|---|---|
| 5.1 | Read `ADC1_DR` idle-powered, while moving freely, and with the horn blocked by hand (briefly) | Roughly ~90–240 counts running, ~830 stalled (`Node-Bus-Power-Path-Spec.md` §3.1.1) | Adjust `stallThresholdCounts` (now 500) in `Damper.h` so it sits clearly between the two |
| 5.2 | Block the horn, command a move | Rail cut within ~200 ms (`stallConfirmMs`); `DamperMode` reports `4`; Fault column shows "Damper stalled"; log line "Damper stall detected" | — |
| 5.3 | Next `DamperTarget` after a stall, also the same value | Stall clears, servo tries again; Fault column drops to "last: Damper stalled" | — |
| 5.4 | 20× full 0 ↔ 100 moves, horn free | Never reports a stall (start-of-move current step doesn't false-trigger) | Raise `stallConfirmMs`, or check the switched-side reservoir cap |
| 5.5 | Scope 5 V and 3.3 V during 5.2 | 5 V dips but recovers; 3.3 V stays in spec; MCU does not reset | Power-path issue (`Node-Bus-Power-Path-Spec.md` §3.1) |
| 5.6 | Moves while the bus is busy | No bus CRC errors or dropped polls on this node or its neighbours | Rail noise into the RS-485 transceiver — decoupling / layout |

## 6. With linkage and damper (2.5:1, 72° blade travel)

| # | Do | Expect | If not |
|---|---|---|---|
| 6.1 | `DamperTarget` 0 | Blade at the minimum-ventilation position, not touching a stop | Mechanical adjustment, not firmware |
| 6.2 | `DamperTarget` 100 | Blade at maximum opening, short of any hard stop | Mechanical adjustment |
| 6.3 | 0 closes, 100 opens | Direction correct | Mount the servo the other way round. If mirrored mounting is ever needed in the field, add a single "reverse" setting then |
| 6.4 | Sweep 0 → 100 in 10 % steps | No stall anywhere in the range | A stall inside the range is a binding linkage |
| 6.5 | `DamperTarget` 50 | Sensible "neutral airflow" position — this is where the node parks before every reset | — |

## 7. Reset and OTA path

| # | Do | Expect | If not |
|---|---|---|---|
| 7.1 | OTA-push the ControllerNode from the server, damper away from 50 % | Damper drives to 50 % (~1.5 s) before the node resets into the bootloader; OTA completes | Server times out waiting for the bootloader → look at the park time vs the server's wait |
| 7.2 | Power-cycle mid-move | Rail off during reset; comes back up unpowered | — |
| 7.3 | Push new firmware twice in a row | Boot counter returns to 0 after each (app healthy) | — |

## 8. Thermostat link (with a Thermostat board)

| # | Do | Expect | If not |
|---|---|---|---|
| 8.1 | Flash a Thermostat with `make -C Software flash-full MODULE=thermostat` only (no provisioning) | Link comes up; `RoomLink` = 1 on the ControllerNode | Check link wiring, bias (R20/R21 on the CN side only) and termination |
| 8.2 | Swap in a second, never-paired Thermostat | Works without any provisioning step (fixed link id) | — |
| 8.3 | Thermostat OTA via the server (`ThermostatFirmware`) | Completes; Thermostat returns on the new version | — |
| 8.4 | Scope I²C on the Thermostat | tLOW ≥ 4.7 µs, tHIGH ≥ 4.0 µs, rise time within Standard-mode limits | Adjust the `TIMINGR` constants in `Lib/HAL/I2c.cpp` |

## 9. Room-loop tuning (observation, not pass/fail)

Once a few rooms run in `Auto`, revisit the defaults listed in `Damper-Budget-Spec.md` §7 item 1 (`roomDeadbandCentiC`, `fullAuthorityCentiC`, `SupplyTemp::staleTimeoutMs`, `BudgetAllocator::recomputeIntervalMs`) against real room behaviour.
