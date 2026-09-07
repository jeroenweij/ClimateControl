# ControllerNode ↔ Thermostat Link — Design Spec

**Status:** Draft — topology and protocol-reuse approach (§2) confirmed 2026-09-06, Thermostat hardware direction set (§4), physical-layer details still open (see §5)
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

## 4. Thermostat hardware

Direction set 2026-09-07. The `Thermostat` reuses the **STM32G030F6P6** and the node-core schematic from `Node-Bus-Hardware-Design-Spec.md` §6 (MCU support parts, reset button, SWD, indicator LED, the `InputPullUp` user-button pattern) — it just swaps the main-bus RS-485 front-end for the point-to-point link (§3) and adds a display + a second button.

### 4.1 Display — I²C OLED

**SSD1306 / SSD1315 128×64 mono OLED.**
- I²C: 2 pins (SDA/SCL), **shared** with the room sensor (§4.3) — no extra pins for the sensor.
- Framebuffer 128×64/8 = **1 KB** of the 8 KB SRAM — fine alongside the link's `Message` buffers.
- **Constraint is flash, not RAM:** 32 KB total. Driver + framing + app fits, but keep fonts minimal (one small + one large digit font, not a font library).

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

- **2 buttons**, each a plain GPIO → GND with the internal pull-up (`Hal::Gpio::Mode::InputPullUp`, active-low) — same wiring as the node `ErrorHandler` ack button. One button *is* that ack button (also used to clear a link-lost error); the second is the UI's set/adjust control. A rotary encoder + push (3 pins) is an alternative if smoother setpoint entry is wanted.
- **Room temperature/humidity sensor: CYBERSEN CHT40MEMS** (`CHT40MEMS`, JLCPCB `C54305346`) on the shared I²C display bus — chosen 2026-09-07. SHT40-clone in the same DFN-4 1.5×1.5 mm footprint, I²C, ±0.2 °C / ±2.5 % RH, −40…+125 °C, ~$0.31/100. 0 extra pins, no ADC calibration. Humidity is worth having on a thermostat (display, and future comfort/dewpoint logic).
  - **Lay out the footprint as the standard SHT40 DFN-4** so a genuine Sensirion `SHT40-AD1F-R2` (`C7461846`) drops onto the same pads — the fallback if CHT40MEMS stock (~1.5k units) or humidity quality disappoints. SHTC3 (`C194656`) is *not* pad-compatible (2×2).
  - **Before committing firmware:** verify from the CHT40MEMS datasheet that it is SHT4x command-compatible (command bytes, CRC-8 poly/init, measurement timing). If so, the existing SHT4x driver just works.
  - **Self-heating is the real design problem.** MCU + LDO + OLED warm the board and a wall thermostat classically reads 1–3 °C high. Mitigate: put the sensor at the *bottom* edge of the PCB (heat rises), far from the LDO/MCU/OLED; mill isolation slots around it (Sensirion app-note "thermal decoupling"); vent holes in the enclosure bottom + top for convection; keep the LDO on the far side of the board. The OLED being off most of the time (§4.2) already removes the biggest heat source. Expect to still need a small firmware offset.
  - This part has no protective membrane — keep flux/outgassing away from it (clean assembly, no conformal coat over the sensor).

### 4.4 Example pin map (STM32G030F6P6, TSSOP20)

| Pin | Signal |
|---|---|
| 1 / 20 | I²C1 SDA (PB7) / SCL (PB6) — OLED + room sensor |
| 9 / 10 | link TX / RX (USART2, PA2/PA3) |
| 8 | link DE (PA1) — only if the link ends up half-duplex RS-485 (§3) |
| 16 / 17 | button 1 / button 2 (PA11 / PA12) |
| 14 | status LED (PA7) |
| 6 / 18 / 19 | NRST + reset button / SWDIO / SWCLK |

~8 of 15 usable GPIO — comfortable headroom.

### 4.5 Power delivery

The display's ~10 mA typical (≤ ~27 mA peak) is trivial over any reasonable feed, so it does not constrain the still-open question of *how* the Thermostat is powered (§5 item 3). If the Thermostat is fed from the `ControllerNode` over the link cable, size that feed for MCU (~5 mA) + transceiver (~1 mA) + OLED (~12 mA typ) ≈ 20 mA, ~40 mA peak.

---

## 5. Open items — need your input before finalizing

1. **Physical link:** half-duplex RS485 (2-wire, matching the main bus parts/BOM for consistency), full-duplex RS485/RS422-style (4-wire), or plain UART (if cable runs are always short, e.g. within one room)?
2. **Cable/connector:** does this link ride on the same RJ45/Ethernet-cable infrastructure as the main bus (`Node-Bus-Hardware-Design-Spec.md` §3, spare conductors?) or a separate cable run entirely? The main-bus RJ45 pinout in that spec is already fully allocated (RS485 pair, 48V pair, GND pair, ENABLE pair) — there's no spare pair for a second differential link on the same cable.
3. **Thermostat power:** does `Thermostat` draw power from `ControllerNode` over this same link/cable (see §4.5 for the current budget), or does it have its own local supply (e.g. mains-adjacent wall power, batteries)? Affects both this spec and a possible future `Thermostat` power-path spec.
4. **Display part number** — §4 locks the room sensor (CHT40MEMS, `C54305346`) and button type; the OLED is still just "SSD1306/SSD1315 128×64" — pick a specific module vs. bare-controller + panel, and confirm no onboard high-Iq LDO on it.
5. **Control loop location:** confirm — does `ControllerNode` itself run the room's thermostat control loop (compare `Thermostat`'s setpoint/room-temp against damper position and act locally), with `MainController` only seeing the results over the main bus? This is the assumption `MainController-Spec.md` §2 is currently built on.
