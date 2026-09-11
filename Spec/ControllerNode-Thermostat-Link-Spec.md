# ControllerNode ↔ Thermostat Link — Design Spec

**Companion docs:** `RS485-Node-Protocol-Spec-STM32G030.md` (main bus protocol this link reuses a subset of), `Node-Message-Model-Spec.md` (endpoint/operation model the link shares; `ThermostatFirmware` is defined here in §5.4), `Node-Flash-Layout-and-Bootloader-Spec.md` (§7 Thermostat OTA — the "relay details" it defers are §5 here), `Software-Architecture-Spec.md` (module map)

---

## 1. Topology

Each `ControllerNode` (damper actuator, main-bus slave) has exactly one `Thermostat` (room UI/setpoint) paired to it. That pair talks over a **separate, dedicated link — not the main RS485 bus**:

```
MainController ──(main RS485 bus, many nodes)── ControllerNode ──(dedicated 2-endpoint link)── Thermostat
                                                  ControllerNode ──(dedicated 2-endpoint link)── Thermostat
                                                  ControllerNode ──(dedicated 2-endpoint link)── Thermostat
                                                       ...                                          ...
```

- `ControllerNode` is on the main bus as a normal `NodeLib` slave (node ID `1..N`, per the protocol spec).
- `Thermostat` is **not** addressable on the main bus at all — it only ever talks to its own `ControllerNode`.
- Because the link has exactly two fixed endpoints, the main protocol's *arbitration* (`Discover`/`Announce`, dynamic roster — protocol spec §6) exists to let one master poll an unknown number of slaves fairly, and that problem doesn't exist here. The `Poll`/`Done` transmit-window discipline is nonetheless kept (§5.3): it costs one round-trip per cycle and buys byte-for-byte reuse of the bus bootloader's OTA slave over this link (§5).

---

## 2. What carries over from NodeLib

| Protocol-spec concept | On this link | Why |
|---|---|---|
| `Id`/`Message` framing, CRC16, sync/resync (protocol spec §3–§5) | **Yes** | A clean, corruption-detected frame is worth having on a 2-endpoint link too — no reason for a different frame format. |
| `Endpoint`/`Operation` enums (`Node-Message-Model-Spec.md`) | **A subset** | The **named-endpoint** blocks `System*`, `Room*` (`RoomSetpoint`/`RoomTemp`/`RoomHumidity`/`RoomMode`), `DamperActual`/`DamperMode`, and `Firmware` — same values as the main bus. Never the `Transport` endpoint. Verbs: `Get`/`Set`/`Report`/`Ack`/`Nack`. The **`Thermostat` is the source of truth for `Room*`** and pushes `Report`s; the `ControllerNode` pushes `DamperActual`/`DamperMode` for the display. See `Node-Message-Model-Spec.md` §7. |
| `Discover`/`Announce` | **Bring-up only** | The CN sends one `Discover` after reset / link loss to read the Thermostat's app-vs-bootloader `state` from its `Announce`. No periodic re-discovery. §5.3. |
| `Poll`/`Done` transmit-window discipline | **Yes — single-peer** | There is nothing to arbitrate, but this is the discipline the bus-resident bootloader's `FirmwareSlave` speaks, so the same bootloader binary takes an image over this link with no OTA-specific code (§5). The CN runs `LinkMaster` — a single-peer master; the cost is one `Poll`/`Done` round-trip per cycle. |
| Heartbeat / `ConnectionLost()` | **Yes** | Driven off the poll cycle (§5.3): 3 consecutive missed `Done` → `ConnectionLost()`. Surfaced on the main bus as `RoomLink` (0 down / 1 up). |

`Software/Lib/NodeLib` is split into a framing layer (`Frame`/`Crc`/`Id`/`Message`/`EEndpoint`/`EOperation`) and the roles built on it. The **Thermostat** runs the full slave (`NodeLib::Node`) — a point-to-point link changes nothing on the slave side. The **ControllerNode** runs `LinkMaster` on the link (§5.3): `NodeMaster` with discovery and the N-slave roster stripped to a fixed single peer, not the full arbitration state machine. Reusing this discipline rather than a bespoke push/ack exchange is what lets the bus bootloader receive an image over the link with no new transfer code.

---

## 3. Physical layer

**Half-duplex 2-wire RS485 with hardware driver-enable** — the same transceiver and BOM as the main bus. Differential signalling suits the duct-to-wall run, and although a 2-endpoint link has nothing to arbitrate and could run full-duplex or plain UART, the bus-resident bootloader's OTA UART (`Modules/Bootloader/OtaUart.cpp`) drives a hardware DE line and expects `DEAT`/`DEDT` turnaround timing. Matching that here — transceiver DE on `Board::Usart2De` (PA1) — is what lets the same bootloader binary serve a Thermostat image with only a USART-select change (§5.5).

On the ControllerNode this is a *second* RS485 front-end (USART2, PA2/PA3, DE PA1) alongside the main-bus one (USART1); on the Thermostat it is the only link. `BoardPins.h` (`LinkUart`) carries these pins. Bit rate is `Board::BusBaudRate` (250 000), the same as the main bus.

### 3.1 Cable, power, and connector

Direction set 2026-09-11: the Thermostat is **fed from the ControllerNode's own regulated 5V rail** (`Node-Bus-Power-Path-Spec.md` §3 — the same LMR16030 buck output that feeds the CN's own 3.3V LDO and, on a ControllerNode, the servo), not raw 48V and not an independent local supply. A local `XC6206P332MR` (`C5446`, same part used board-wide) drops it to 3.3V on the Thermostat side.

- **4 conductors, no separate ENABLE.** `RS485 A`, `RS485 B`, `+5V`, `GND` — that's it. A separate power-kill line from CN to the Thermostat was considered and dropped: CN can already reset the Thermostat over the link (`Firmware[EnterBootloader]`, §5), a physical reset button already exists on the Thermostat board (inherited from the node-core schematic, §4), and a hung link already parks the damper safely without needing to power-cycle anything (§5.1). Whatever *would* need a hard power-cycle to recover is already covered when the main bus's shared `ENABLE` kills this CN's own buck — the Thermostat's feed collapses with it for free, since it's downstream of the same rail. Independently killing just the Thermostat while the CN stays up solves a problem that doesn't currently exist; adding it would cost a 5th conductor plus a CN-side load switch (`Node-Bus-Power-Path-Spec.md` §3.1-style P-FET) for no identified benefit.
- **Voltage drop margin is not a constraint.** At the documented worst-case load (§4.5, ~40mA peak) and the `XC6206`'s datasheet-worst-case dropout (350mV @ 100mA — conservative, since our load is well under that test point), the cable can run to roughly 125–200m (26AWG–24AWG stranded) before the far-end LDO loses regulation, using CN's ~5.0V rail as the source. That's far beyond any real duct-to-wall or wall-to-CN run; RS485 itself (several hundred meters at 250kbps, point-to-point, no multidrop reflections to worry about) isn't the limiting factor either. Cable routing and connector cost are what actually bound the practical run, not electronics.
- **Connector: JST-XH, 2.5mm pitch, 4-position** — chosen over RJ45 specifically to keep the Thermostat enclosure compact (§4.6); RJ45's ~16×13.5×20mm mating envelope would blow out the puck's thickness budget, where an XH header mounts flat on the PCB and only needs a small cutout for the plug nose.

  | Role | Part | LCSC |
  |---|---|---|
  | PCB header (both ends) | `B4B-XH-A(LF)(SN)` | `C144395` |
  | Cable-side housing | `XHP-4` | `C144403` |
  | Crimp socket contacts (×4/cable end) | `SXH-001T-P0.6` | `C140573` (standard, not the `-N` low-insertion-force variant — this connector is installed once and left, so favor vibration retention over easy insertion) |

  3A/250V rated (far past our ~40mA), 22–28 AWG wire, −25…+85°C — the *standard* compact XH family (9.8mm mounting height), not JST's bulkier "high box" potted-board variant, which shares a confusingly similar `XHP-n` numbering but is taller and meant for a different application. **Verify the friction latch before ordering at volume:** LCSC's own listing metadata tags `XHP-4` as "non-latching," which conflicts with XH's usual friction-latch reputation and isn't contradicted anywhere in JST's datasheet — check a product photo or a sample part.

### 3.2 A/B line passives — reuses the main-bus scheme (`Node-Bus-Hardware-Design-Spec.md` §6.3), same transceiver

The link uses the same `MAX3485CSA-JSM` (LCSC `C6395158`) as the main bus, so the same passives apply — but simpler, because this link's topology never varies: it is always exactly `ControllerNode` at one end and `Thermostat` at the other, so there's no need for the main bus's DNP-plus-jumper "populate only wherever the physical ends happen to be" scheme. Populate directly:

| Part | Value | Populate |
|---|---|---|
| Termination | 120 Ω across A–B | **both ends, always** — CN's link-side transceiver and the Thermostat's, permanently populated (not DNP; unlike the main bus, which end is "the end" never changes here) |
| Fail-safe bias | A→3V3, B→GND, ~560 Ω each | **once for the whole link, at ControllerNode** (the link's master, `LinkMaster`, §5.3) — not at the Thermostat, and not both: a second bias network would just parallel down and skew the idle point for no benefit. `MAX3485` is not true-fail-safe, same as the main bus. **Built 2026-09-11**: `R20`/`R21`, `0603WAF5600T5E`, LCSC `C23204` (Basic part) — R20 A→3V3, R21 B→GND, confirmed on the CN-side link net in the netlist. Actual current draw is ~2.8mA (3.3V across the 560+560Ω pair plus the ~60Ω of parallel A-B termination at both ends) — negligible against the rail. |
| Series R | 10 Ω in each of A/B | both ends, recommended (not just optional as on the main bus) — the 4-conductor cable here (§3.1) isn't purpose-made 120 Ω-rated RS485 cable the way the main bus's Ethernet patch cable is treated, so the extra ringing/EMI damping is more likely to matter |
| ESD/surge | SM712 RS-485 TVS, SOT-23-3, pin 1→A, pin 2→B, pin 3→GND | both ends, right at the connector — LCSC `C5199207` (ElecSuper) or `C404012` (genuine Bourns), same as every main-bus node. The Thermostat's connector sits at an exposed wall location, if anything more ESD-exposed than an enclosed duct node. |
| DE/RE pull-down | 10 kΩ, DE/RE net → GND | both ends, mandatory — same reset-safety reason as the main bus: an unprogrammed or resetting node must not float its driver onto the line. |

---

## 4. Thermostat hardware

Direction set 2026-09-07. The `Thermostat` reuses the **STM32G031F8P6** (project MCU, locked in 2026-09-08) and the node-core schematic from `Node-Bus-Hardware-Design-Spec.md` §6 (MCU support parts, reset button, SWD, indicator LED, the `InputPullUp` user-button pattern) — it just swaps the main-bus RS-485 front-end for the point-to-point link (§3) and adds a display + a second button.

### 4.1 Display — I²C OLED

**SSD1306 / SSD1315 128×64 mono OLED.** Part locked 2026-09-11: **Wisevision `X096-2864KSWPG01-H30`** (LCSC `C18723026`), ~$1.86/1 down to ~$1.09 at volume, 955 units in stock at time of selection — re-check stock before a production-size order.
- **True bare COG module** — SSD1315 chip bonded directly to the glass, 30-pin 0.7 mm-pitch FPC tail exposing every controller pin, no PCB, no onboard regulator of any kind. This is what the power table below assumes; it fully satisfies the "bare-controller module" requirement that was open in §6.
- **Panel:** 24.7×16.6×1.3 mm outline, 21.74×11.175 mm active area, white — comfortably inside the §4.6 puck enclosure, and white reads more neutral than the common blue hobbyist panels behind a matte-white front.
- **I²C pin strapping** (per datasheet §1.5 Pin Definition): `BS0=0, BS1=1, BS2=0` (tie to `VSS`/`VDD`/`VSS`) selects I²C mode; then `CS#→VSS`, `R/W#→VSS`, `E/RD#→VSS` (all tied low, per the datasheet's serial/I²C note); `D0→SCL`, `D1` and `D2` **tied together→SDA** (the controller uses separate internal SDA-in/SDA-out pins that must be shorted externally); `D3–D7` unused, tie to `VSS`. `D/C#` doubles as the I²C slave-address bit `SA0` — **pick whichever ties (`VSS`/`VDD`) doesn't collide with the CHT40MEMS's fixed address on the same shared bus (§4.3)**, check both datasheets before laying out.
- Also needs, per the datasheet's application circuit: `RES#` driven by an MCU GPIO or simple RC (pull high for normal operation), an `IREF` resistor to `VSS` (segment current reference, ≤ 12.5 µA), a cap from `VCOMH` to `VSS`, and — using the internal DC/DC charge pump so no separate ~7.5–15 V panel rail is needed — `VBAT` tied to the same `VDD`/3.3 V rail with the `C1P/C1N/C2P/C2N` flying capacitors populated per that circuit (size the resistor/caps from the datasheet's own example rather than guessing; not reproduced here). Pins `1`/`30` (N.C., support pins) must still be tied to ground for ESD.
- I²C: 2 pins (SDA/SCL), **shared** with the room sensor (§4.3) — no extra pins for the sensor.
- Framebuffer 128×64/8 = **1 KB** of the 8 KB SRAM — fine alongside the link's `Message` buffers.
- **Constraint is flash, not RAM:** the app slot is 50 KB (`Node-Flash-Layout-and-Bootloader-Spec.md` §3). Driver + framing + app fits, but keep fonts minimal (one small + one large digit font, not a font library).

**Power** (module with onboard charge pump, from 3.3 V):

| State | Current | Note |
|---|---|---|
| off (`0xAE` sleep) | < 10 µA (bare controller) | cheap modules add ~0.05–5 mA from an onboard LDO — use a bare-controller module or drive the SSD1306 chip directly |
| on, blank | ~3–5 mA | |
| on, typical UI (~15 % pixels) | ~8–12 mA | |
| on, all-white max contrast | ~20–27 mA | worst case |

### 4.2 Display wake — button press

**No motion/PIR sensor** (rejected 2026-09-07 on BOM cost). The display is woken by a **button press**; after an inactivity timeout it dims (contrast register) then turns off (`0xAE`). Averaged over realistic use the OLED contributes < 0.1 mA — it effectively leaves the power budget, and the screen-off state also avoids burn-in of the static digits. Ignore the "screen dark until touched" UX cost.

*Optional:* an ambient-light sensor (phototransistor on an ADC pin, or an I²C ALS on the shared bus) to drop OLED contrast in a dark room — near-zero added cost, not required.

### 4.3 Buttons & room sensor

- **2 touch buttons, capacitive** — direction set 2026-09-11, driven by a **Holtek BS212C-1** touch-key IC (LCSC `C42372571`, SOT-23-6) rather than mechanical switches, to support the sealed-enclosure look of §4.6 (copper mesh behind the plastic front as the two electrodes, no physical hole/actuator through the case).
  - `KOUT1`/`KOUT2` are NMOS, active-low, with internal pull-high — the same polarity `Hal::Gpio::Mode::InputPullUp` already assumes for a plain switch-to-GND button, so they wire straight onto the same two MCU GPIOs (`PA11`/`PA12`, §4.4) with **no firmware change**. One channel keeps the node `ErrorHandler` ack role (also clears a link-lost error); the other is the UI's set/adjust control — same mapping as the mechanical design it replaces.
  - `KEY1`/`KEY2` route to the two copper-mesh electrode pads. Per the datasheet, sensitivity is set by electrode/copper area, the plastic's thickness, and — the easy knob during bring-up — a per-channel 0–25 pF capacitor footprint on each `KEY` pin (`Ct`; higher Ct = lower sensitivity, 0 pF = max). Leave the footprint unpopulated until the enclosure's actual plastic thickness is known, then tune empirically.
  - **Auto-calibration** (power-on, and again after ~1 s idle in normal mode / ~2 s in standby, per datasheet) re-baselines against drift from temperature, humidity and aging of the mesh/adhesive — the reason this part was picked over the touch ICs on the shortlist without it. A stuck-key timeout (60–68 s max key-on time) forces a re-cal if something covers a pad continuously.
  - Power: ~3.5 µA (3 V) standby, ~0.6 mA typ / 0.9 mA max (3 V) while active — negligible against the §4.5 budget. 0.1 µF decoupling on `VDD` per the datasheet's application circuit; no external LDO needed (the part has its own adaptive voltage-drop immunity to supply noise).
- **Room temperature/humidity sensor: CYBERSEN CHT40MEMS** (`CHT40MEMS`, JLCPCB `C54305346`) on the shared I²C display bus — chosen 2026-09-07. SHT40-clone in the same DFN-4 1.5×1.5 mm footprint, I²C, ±0.2 °C / ±2.5 % RH, −40…+125 °C, ~$0.31/100. 0 extra pins, no ADC calibration. Humidity is worth having on a thermostat (display, and future comfort/dewpoint logic).
  - **Lay out the footprint as the standard SHT40 DFN-4** so a genuine Sensirion `SHT40-AD1F-R2` (`C7461846`) drops onto the same pads — the fallback if CHT40MEMS stock (~1.5k units) or humidity quality disappoints. SHTC3 (`C194656`) is *not* pad-compatible (2×2).
  - **Before committing firmware:** verify from the CHT40MEMS datasheet that it is SHT4x command-compatible (command bytes, CRC-8 poly/init, measurement timing). If so, the existing SHT4x driver just works.
  - **Self-heating is the real design problem.** MCU + LDO + OLED warm the board and a wall thermostat classically reads 1–3 °C high. Mitigate: put the sensor at the *bottom* edge of the PCB (heat rises), far from the LDO/MCU/OLED; mill isolation slots around it (Sensirion app-note "thermal decoupling"); vent holes in the enclosure bottom + top for convection; keep the LDO on the far side of the board. The OLED being off most of the time (§4.2) already removes the biggest heat source. Expect to still need a small firmware offset.
  - This part has no protective membrane — keep flux/outgassing away from it (clean assembly, no conformal coat over the sensor).

### 4.4 Example pin map (STM32G031F8P6, TSSOP20)

| Pin | Signal |
|---|---|
| 1 / 20 | I²C1 SDA (PB7) / SCL (PB6) — OLED + room sensor |
| 9 / 10 | link TX / RX (USART2, PA2/PA3) |
| 8 | link DE (PA1) — half-duplex RS-485 (§3) |
| 16 / 17 | `KOUT1`/`KOUT2` from the BS212C-1 touch IC — was button 1/2 (PA11 / PA12) |
| 14 | status LED (PA7) |
| 6 / 18 / 19 | NRST + reset button / SWDIO / SWCLK |

~8 of 15 usable GPIO — comfortable headroom.

### 4.5 Power delivery

The display's ~10 mA typical (≤ ~27 mA peak) is trivial over any reasonable feed. The Thermostat is fed from the `ControllerNode` over the link cable (§3.1); size that feed for MCU (~5 mA) + transceiver (~1 mA) + OLED (~12 mA typ) ≈ 20 mA, ~40 mA peak.

### 4.6 Enclosure — industrial design direction

Direction set 2026-09-11: style the enclosure after the **IKEA TIMMERFLOTTE** (temp/humidity sensor, ~65×65×18 mm puck) — small rounded disc, matte white, no visible branding/text on the face, nothing on the wall/shelf reading as "a gadget." Concretely, for whoever designs the case:

- **Form factor:** round puck, roughly TIMMERFLOTTE's ~65 mm width class in plan; depth driven by our stack-up (PCB + OLED module + standoffs), which is very likely thicker than TIMMERFLOTTE's 18 mm once the connector/cable entry (below) is accounted for — treat 18 mm as an aspiration, not a constraint to force.
- **Finish:** matte white (or matte black to match room trim — pick one per install, not both), no exposed screws on the visible face, seams at the back/rim rather than front.
- **Face:** the OLED window is the only thing that reads on the front; buttons should sit flush or nearly flush rather than protruding, in keeping with TIMMERFLOTTE's "the whole thing feels like one big button" feel. Reviewers note it has *no* auto-lit display — you press to see the reading — which already matches this spec's §4.2 wake-on-press behavior, so the visual language and the interaction we already designed agree.
- **Mount:** wall-mount (this is a fixed room thermostat, not a shelf sensor) — a rear plate/bracket the puck clips onto, screw holes hidden behind it.

**Two real deltas from a literal copy:**
- TIMMERFLOTTE is battery-powered and cable-free; this Thermostat is wired — link plus power, both on the same 4-conductor cable (§3.1, now decided) — so the enclosure needs a cable entry TIMMERFLOTTE doesn't have — round back, cable out the rear/bottom so the front stays clean.
- TIMMERFLOTTE's whole face is *one* button (read-only device — press to cycle temp/humidity). This Thermostat also drives a setpoint (§4.3, 2 touch buttons), so a literal one-big-button face doesn't carry over as-is without an interaction redesign — still deliberately left for a future pass, not decided here.

---

## 5. Firmware update — the OTA link master

The same bus-resident bootloader binary receives the Thermostat image over this link, unchanged, because the ControllerNode speaks the transport discipline the bootloader already implements. This is the "relay details" that `Node-Flash-Layout-and-Bootloader-Spec.md` §7 defers here.

### 5.1 Ordering — the node first, then its Thermostat

A ControllerNode and its Thermostat update as two separate, sequential jobs, CN first:

1. The **ControllerNode** updates over the main bus — the normal flow of `Node-Flash-Layout-and-Bootloader-Spec.md` §6. Self-healed by the CN's own bootloader if interrupted.
2. Once the CN is confirmed back in its application on the new image, the **Thermostat** update runs *through the CN application*, which acts as the OTA master on the link (§5.4).

The CN **bootloader never relays** — it only ever updates the CN itself. A Thermostat is updatable only while its CN runs a healthy application; that CN app is the Thermostat's recovery anchor, in place of "the bus" for a main-bus node. A Thermostat left with an invalid image sits in its bootloader waiting for its CN to re-drive the push — no J-Link needed as long as the CN app and the link are intact. Interrupting a Thermostat push does not disturb the CN (it is relaying, not resetting itself), so the server just retries.

**Damper safe state during the transfer.** Whenever the CN loses fresh room data — its Thermostat sitting in the bootloader for a transfer, or any `ConnectionLost()` (§5.3) — the CN drives the damper to **50 % (neutral airflow)** and then powers the servo down. It does not hold the last position or fall back to `DamperMode`. This is the same park the CN runs in `INodeHandler::PrepareForReset()` before its *own* OTA reset. The servo is unpowered by default in the hardware — a STM32-gated power enable, de-asserted at reset and while the MCU is unprogrammed — and is energised only for the brief move to a new setpoint, so "powered down" is the resting state and the metal gearing holds 50 % until the Thermostat re-announces and the control loop resumes. Hardware side: `Node-Bus-Power-Path-Spec.md` §3.1.

### 5.2 The Thermostat runs full NodeLib

The Thermostat application is an ordinary `NodeLib::Node` slave on its link USART (USART2, PA2/PA3 + PA1 DE), with `ConfigStore` giving it `module = Thermostat` and its `nodeId` (§5.2.1). It serves the §2 endpoint subset (`System*`; `Room*` as source of truth; `DamperActual`/`DamperMode` for the display; `Diagnostics*`; `Firmware`). The point-to-point nature of the link changes nothing on the slave side — it is effectively a `ControllerNode` app minus the damper, plus the OLED/sensor/buttons.

`Firmware` on the Thermostat is handled as on any node: a running app receiving `Firmware[EnterBootloader]` parks its UI, writes `Board::EnterBootloaderMagic` to the backup register and resets; the bootloader then serves the `Begin`/`Write`/`End`/`Activate` transfer.

#### 5.2.1 Thermostat `nodeId` — same as its ControllerNode

The Thermostat is provisioned with the **same `nodeId` as the ControllerNode it is paired to**. The pair is programmed together at manufacture — identical `nodeId`, different `module` (`1` = ControllerNode, `4` = Thermostat). The link is private, so the shared id never collides: the CN acts only as master on the link, the Thermostat only as slave.

Consequences:
- A Thermostat is **not field-interchangeable** without re-provisioning.
- The server, `0x63` and `ota_jobs` identify a Thermostat by its owning ControllerNode's id directly; no separate address space.
- `FirmwareSlave`'s `(nodeId-1)×25 ms` announce back-off is dead time on the 1:1 link — `LinkMaster` just waits out its discovery window.
- `LinkMaster`'s single peer id is `ConfigStore::NodeId()` (the CN's own id).
- The `provision` CMake target (`Node-Flash-Layout-and-Bootloader-Spec.md` §6.3) has a pair mode that writes both records with a shared id in one bench step.

### 5.3 `LinkMaster` — the CN's single-peer poll loop

The CN application instantiates, on its link USART, `NodeLib::LinkMaster` — a distinct class, not a mode of `NodeMaster`. It shares only the framing/`Node` plumbing; the discovery array, dynamic roster and round-robin cursor are not carried. It is `NodeMaster` with the arbitration removed:

| `NodeMaster` | `LinkMaster` |
|---|---|
| dynamic discovery, `activeNodes[MAX_NODES]` | fixed single peer, id = `ConfigStore::NodeId()` (§5.2.1) |
| round-robin across N slaves | poll the one peer every **200 ms** |
| `Discover` + Announce collection each cycle | one `Discover` at bring-up / after link loss to read the peer's app-vs-bootloader `state`; no periodic re-discovery |
| injects master `Set`/`Get` in the gaps | same |
| `ConnectionLost()` from poll-cycle bookkeeping | **3** consecutive missed `Done` → `RoomLink = 0` + `ConnectionLost()` |

Normal traffic: the Thermostat pushes `Room*` `Report`s in its poll window; the CN injects `Set DamperActual` / `Set DamperMode` for the display, and `Set RoomSetpoint` for a master override. One poll loop covers liveness, room-state relay and OTA.

### 5.4 `ThermostatFirmware` endpoint (`0x22`)

Delegating the Thermostat's OTA (and version query) from the main bus needs one new endpoint, because a plain `Firmware` frame addressed to the CN means "update the CN". `ThermostatFirmware = 0x22` sits in the Firmware block of `Node-Message-Model-Spec.md` §3 — the `FirmwareOp` sub-opcode symmetry with `Firmware = 0x20` is the reason it is not in the `Room` block.

```
ThermostatFirmware = 0x22   // data[0] = FirmwareOp; "act on my paired Thermostat over the link"
```

- Same `data[0] = FirmwareOp` sub-opcodes as `Firmware` (`Begin`/`Write`/`End`/`Activate`/`Abort`/`Status`) — the `Operation` verb set does not grow.
- **NodeLib does not self-handle it** (unlike `Firmware`). It is delivered to the ControllerNode's `INodeHandler` like an application endpoint — only the ControllerNode module defines it, exactly like the `Room*` block.
- The CN handler terminates each frame and originates a fresh link transaction against the Thermostat's real `Firmware` endpoint, driven by `LinkMaster`:

| main bus → CN | CN → Thermostat, over the link |
|---|---|
| `Set ThermostatFirmware[Begin] {module=4, imageSize, imageCrc32, fwVersion, flags}` | (guard, §5.4.1) `Set Firmware[EnterBootloader]`, wait for the bootloader `Announce`, then `Set Firmware[Begin {…}]` (the standard 12-byte payload, `flags` dropped) |
| `Set ThermostatFirmware[Write] {offset, bytes≤27}` | `Set Firmware[Write {offset, bytes}]` in the next link poll window |
| `Set ThermostatFirmware[End]` / `[Activate]` / `[Abort]` | the same `Firmware` op |
| `Get ThermostatFirmware` (no FirmwareOp) | answered from the CN's link cache — no link traffic |

- `ThermostatFirmware[Begin]` is CN-terminated, so its payload differs from the bus `Firmware[Begin]` — it adds a `flags` byte (bit 0 = `Force`, §5.4.1), ~14 bytes, well under `MAX_DATA`.
- The CN `Report`s `ThermostatFirmware {FirmwareOp::Status, state, expectedOffset, lastError, fwVersion}` up the main bus, copied from the Thermostat's link `Status` (or from cache for a bare `Get`). This carries the Thermostat's running firmware version on demand — no separate "thermostat info" endpoint.
- The CN keeps a small link cache of the Thermostat's `state` + `fwVersion` (+ `uid`, §5.6), refreshed by a periodic link `Get SystemInfo` / `Get Firmware`, so `Get ThermostatFirmware` is always answerable.
- One 27-byte chunk crosses the CN at a time — no image staging. The CN does **not** forward frames between the two buses (`Node-Message-Model-Spec.md` §7): it terminates and re-originates.

#### 5.4.1 Already-current guard

With `Force` (bit 0 of `flags`) clear, if the CN's link cache shows the Thermostat already running the requested `fwVersion`, the CN does not disturb it: it skips `EnterBootloader` and immediately `Report`s `Status {state = app, lastError = AlreadyCurrent}`. A working Thermostat is never rebooted for a no-op update.

The server makes the same check *before* creating the job — it has the Thermostat's version from `0x63` and the target from the uploaded image descriptor — and warns the operator rather than starting a pointless transfer. The CN guard is the backstop for a stale server cache. `Force` is set only on an explicit operator "re-flash anyway". Match is on `fwVersion` only (the cached `SystemInfo` carries no `buildId`).

### 5.5 Bootloader — one binary, USART select by module

`Modules/Bootloader/OtaUart.cpp` picks its USART and pins from the provisioned module type:

| `ConfigStore::GetModule()` | link USART | TX / RX | DE |
|---|---|---|---|
| `Thermostat` | USART2 | PA2 / PA3 (AF1) | PA1 (AF1) |
| everything else | USART1 | PB6 / PB7 (AF0) | PA12 (AF1) |

Half-duplex 2-wire RS485 with hardware driver-enable (`USART_CR3_DEM`, `DEAT`/`DEDT`) in both cases — identical framing, baud and `FirmwareSlave` logic. `main.cpp`'s `StayResident()` routes a provisioned node (`ConfigStore::Valid()`) to `FirmwareSlave(BusBaud, NodeId(), module)`; a provisioned Thermostat lands there and serves the transfer on USART2. The "one bootloader binary, all four boards" property and the Thermostat pin map of §4.4 are unchanged.

### 5.6 Server & MainController awareness

The server models every `ControllerNode` as owning one Thermostat.

- **Presence / version / identity:** uplink endpoint `0x63 ThermostatStatus`, MC→S `Report {controllerNodeId(1), linkUp(1), blState(1), fwMajor(1), fwMinor(1), uid[12]}` (17 B), on change + slow keepalive. The MC fills it from `Get RoomLink` + `Get ThermostatFirmware` on each ControllerNode.
  - **`uid`** is the Thermostat MCU's 96-bit factory device ID (read-only at `0x1FFF_7590`, `Node-Flash-Layout-and-Bootloader-Spec.md` §2). The CN reads it once from the Thermostat's `SystemInfo` (`uid[12]`) over the link and caches it. Same role as `nodes.uid` for bus nodes: it names the physical unit independent of the assigned `nodeId`, so the server can tell a Thermostat behind a given CN was physically swapped (new `uid`, same id). Inventory / OTA-history signal, not used for routing.
- **OTA target:** `0x65 OtaControl` Set gains no new field — `module == Thermostat (4)` in the payload means "`targetNodeId` is the owning ControllerNode; drive `ThermostatFirmware`, not `Firmware`". `0x66 OtaData` is byte-identical; the MC wraps each chunk into `ThermostatFirmware[Write]`. The `Force` flag (§5.4.1) rides in the `0x65` payload's spare byte.
- **DB:** `thermostats(controller_node_id PK → nodes.id, module, uid, fw_version, bl_state, link_up, last_seen)`; `ota_jobs` has `target TEXT NOT NULL DEFAULT 'node'` (`'node'` | `'thermostat'`), `node_id` staying the bus node id in both cases.
- **UI:** the Status page lists each Thermostat under its ControllerNode with its own fw version and an upload control; the building-map per-room badge renders `RoomLink`.

### 5.7 Implementation status

Built in firmware: the `EEndpoint`/`EFirmware` additions, the `NodeLib` framing use split (`LinkMaster` added alongside `Node`/`NodeMaster`), `OtaUart` module-aware USART select, and the `ControllerNode` and `Thermostat` modules — the ControllerNode carries the `Damper`, the Room* cache, `LinkMaster`, and the `ThermostatFirmware` relay; the Thermostat is a NodeLib slave on the link with the OLED / CHT40 sensor / buttons still stubbed (they need an I²C HAL).

Built in the `Webserver`: `ota_jobs.target` (`'node'` | `'thermostat'`); the
`0x63 ThermostatStatus` decode + `thermostats` table (`controller_node_id`,
`uid`, `fw_version`, `bl_state`, `link_up`, `last_seen`); the `firmware_images`
repository (one image per module, module + version parsed from the upload
filename `<Module>_<major>.<minor>.bin` and cross-checked against the descriptor,
which the build now fills from `CC_FW_VERSION`); the **Firmware** page, which
lists every node with its installed version against the held image and gives
each ControllerNode a second row for its Thermostat, plus per-node and
per-module ("update all") push buttons that grey out when the target is offline
or already current; and a single-flight OTA **queue** (one push at a time, the
rest `state = 'queued'`, fed by both single presses and "update all"). Node
firmware versions come from `SystemInfo` reports, thermostat versions from
`0x63`.

Not built yet: the MainController side (emitting `0x63 ThermostatStatus`,
`module == 4` routing in the OTA sequence — waits on the MainController uplink
layer as a whole), the `provision` target's pair mode, the pre-flight
already-current check against `0x63` before creating a thermostat job, and
`Force` re-flash.

---

## 6. Open items

1. **Control loop location:** confirm — does `ControllerNode` itself run the room's thermostat control loop (compare `Thermostat`'s setpoint/room-temp against damper position and act locally), with `MainController` only seeing the results over the main bus? This is the assumption `MainController-Spec.md` §2 is currently built on.
