# ClimateControl

C++ firmware for an HVAC climate-control system: one central `MainController` plus distributed RS485 nodes, including damper-actuating `ControllerNode`s (metal geared servo) and duct-mounted `TemperatureNode`s, each `ControllerNode` paired with a room `Thermostat`. All targets are STM32 microcontrollers, built with CMake and flashed via SEGGER J-Link.

See `Spec/` for the full design docs — read those before making architectural changes; this file is a quick orientation, not a substitute.

## Project structure

```
ClimateControl/
├── Spec/                   # design specs — protocol, hardware, power, per-module, architecture
├── Hardware/                # PCB/schematic sources
└── Software/
    ├── .clang-format       # copied verbatim from ~/git/rollercoaster — do not diverge, same style everywhere
    ├── CMakeLists.txt      # top-level CMake project — aggregates Lib/* (and Modules/* once they exist)
    ├── cmake/              # ARM Cortex-M0+ toolchain file
    ├── Lib/
    │   ├── HAL/             # thin wrapper around STM32Cube HAL/LL — the only place ST driver headers get included
    │   ├── Board/           # BoardPins.h — single source of truth for the STM32G030 pin map (header-only)
    │   ├── Tools/           # DelayTimer + Logger — shared helpers
    │   └── NodeLib/          # RS485 v2 protocol library (Node/NodeMaster/Id/Message/ChannelId/Operation)
    └── Modules/
        ├── MainController/   # RS485 bus master
        ├── ControllerNode/   # damper/servo slave node + ControllerNode<->Thermostat link
        ├── TemperatureNode/  # duct temperature slave node
        └── Thermostat/       # room UI, paired 1:1 to one ControllerNode
```

Full rationale for this layout is in `Spec/Software-Architecture-Spec.md`. `Modules/*` don't exist yet.

## System architecture

- **Main RS485 bus:** `MainController` (node ID `0`, bus master) plus `ControllerNode` and `TemperatureNode` as slaves (IDs `1..N`), all speaking the `NodeLib` v2 protocol — see `Spec/RS485-Node-Protocol-Spec-STM32G030.md` for the wire format (variable-length frame, CRC16, hardware USART DE) and `Spec/Node-Bus-Hardware-Design-Spec.md` / `Spec/Node-Bus-Power-Path-Spec.md` for the shared 48V RJ45 physical layer.
- **Thermostat link:** each `ControllerNode` talks to its own `Thermostat` over a *separate, dedicated point-to-point link*, not the main bus — `Thermostat` is never addressable from `MainController`. Confirmed: this link reuses `NodeLib`'s `Id`/`Message`/CRC framing but **not** the main bus's round-robin polling/discovery state machine (`NodeMaster`) — no arbitration is needed between two fixed endpoints, so `NodeLib` should be split into a standalone framing layer plus the bus-master layer built on it. See `Spec/ControllerNode-Thermostat-Link-Spec.md`.
- Many architectural decisions across all of the above are still open (sensor parts, physical link details, MainController's outward-facing role, buffer sizes, baud rate) — check each spec's "Open items" section before assuming an answer.

## Code style reference

This project's C++ style follows `~/git/rollercoaster` and its `node` submodule (`~/git/node`) — that codebase is the AVR/Arduino predecessor this project is conceptually porting to STM32 (see `RS485-Node-Protocol-Spec-STM32G030.md`, which explicitly supersedes `~/git/node/Software/lib/NodeLib`). `Software/.clang-format` is copied verbatim from there.

Conventions (from that codebase, not all auto-enforced by clang-format):
- Allman braces, 4-space indent, no tabs, `#pragma once`.
- `PascalCase` classes/methods/enum values; `camelCase` members/locals; no `m_`/`_` prefixes.
- `enum class` for protocol/state types, each with an `operator<<(std::stringstream&, ...)` for logging.
- Constructors: member-initializer list, one member per line after the colon.
- `namespace Foo { ... } // namespace Foo` — always comment the closing brace.
- `.cpp` files pull in needed symbols via `using NodeLib::Foo;` near the top rather than `using namespace`.
- No heap allocation in protocol/channel code — fixed-size buffers/queues, matching the RAM budget discipline in the RS485 spec (§7).
- File header comment:
  ```cpp
  /*************************************************************
  * Created by J. Weij
  *************************************************************/
  ```

When porting a class from `~/git/node/Software/lib/NodeLib` or `~/git/node/Software/src`, keep the class/method shape and naming; only the platform layer underneath changes (Arduino `pinMode`/`digitalWrite`/`Serial1`/`millis` → `Lib/HAL` wrapping STM32Cube HAL/LL).

## Build & toolchain

- **Compiler:** `arm-none-eabi-gcc` (installed on this machine).
- **Build system:** CMake — one `CMakeLists.txt` per `Modules/*` producing a `.elf`, top-level `Software/CMakeLists.txt` aggregating `Lib/*` and `Modules/*` (toolchain file in `Software/cmake/`). No STM32 CMake toolchain file exists yet in any sibling repo; it needs to be written from scratch (target triple `-mcpu=cortex-m0plus -mthumb`, linker script, startup file).
- **Driver layer:** STM32Cube HAL/LL, wrapped by `Lib/HAL` — protocol and application code never includes ST headers directly.
- **MCU:** STM32G031F8P6 (Cortex-M0+, 64 MHz, 64 KB flash / 8 KB SRAM, TSSOP20) — locked in 2026-09-08 for all four boards (MainController, ControllerNode, TemperatureNode, Thermostat). Drop-in replacement for the earlier STM32G030F6P6TR: identical pinout, +32 KB flash (bus-resident DFU bootloader + NINA driver headroom), plus LPUART1 / RTC+backup-registers / TIM2. HAL device define is `STM32G031xx`.
- **Flashing:** SEGGER J-Link — a CMake custom target per module shells out to `JLinkExe` with a generated commander script. **`JLinkExe`/`JLinkGDBServer` are not yet installed on this machine** — install the J-Link Software Pack before the flash target will run.

## Specs index

| Doc | Covers |
|---|---|
| `RS485-Node-Protocol-Spec-STM32G030.md` | Main bus wire protocol (frame format, CRC, framing/resync, buffering) |
| `Node-Flash-Layout-and-Bootloader-Spec.md` | Flash partition map, bus-resident OTA bootloader (speaks NodeLib), app image format, persistent `NodeId`/config store |
| `Node-Message-Model-Spec.md` | Message header rethink: `channel`→`endpoint` (flat named-thing enum in blocks), redesigned `operation` verbs, value encodings |
| `Node-Bus-Hardware-Design-Spec.md` | Main bus physical layer (48V PoE-class power, RJ45 pinout, connector part) + node core schematic (§6: MCU support, transceiver, LEDs, buttons, pin plan) |
| `Node-Bus-Power-Path-Spec.md` | Per-node 48V→5V→3.3V regulation chain |
| `Software-Architecture-Spec.md` | Module map, directory layout, build/toolchain, code style |
| `MainController-Spec.md` | Bus-master role + 48V power input / bus injection (§3); NINA-W152 Wi-Fi connectivity (§5); what it does with the data still open |
| `TemperatureNode-Spec.md` | Duct temperature sensing node; sensor choice still open |
| `ControllerNode-Thermostat-Link-Spec.md` | Per-room point-to-point link + Thermostat hardware (§4: G030 + I²C OLED + 2 buttons); link physical layer still open |
