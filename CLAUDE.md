# ClimateControl

C++ firmware for an HVAC climate-control system: one central `MainController` plus distributed RS485 nodes, including damper-actuating `ControllerNode`s (metal geared servo) and duct-mounted `TemperatureNode`s, each `ControllerNode` paired with a room `Thermostat`. All targets are STM32 microcontrollers, built with CMake and flashed via SEGGER J-Link.

See `Spec/` for the full design docs — read those before making architectural changes; this file is a quick orientation, not a substitute.

## Project structure

```
ClimateControl/
├── Spec/                   # design specs — protocol, hardware, power, per-module, architecture
├── Hardware/                # PCB/schematic sources
├── Webserver/              # Go building server: MainController TCP uplink → SQLite + embedded SPA
└── Software/
    ├── .clang-format       # copied verbatim from ~/git/rollercoaster — do not diverge, same style everywhere
    ├── CMakeLists.txt      # top-level CMake project — aggregates Lib/* and Modules/*
    ├── cmake/              # ARM Cortex-M0+ toolchain file + stm32.cmake (add_stm32_executable)
    ├── test/               # host-native unit-test project (framework + Hal fakes); suites live per-module in Lib/*/test and Modules/*/test
    ├── Lib/
    │   ├── HAL/             # thin wrapper around STM32Cube HAL/LL (Uart/Gpio/Crc/Flash/Backup/Tick/System/OneWire) — the only place ST driver headers get included
    │   ├── Board/           # BoardPins.h (pin map) + MemoryMap.h / ImageDescriptor.h (flash layout) — header-only
    │   ├── Tools/           # DelayTimer + Logger — shared helpers
    │   ├── Startup/         # shared startup_stm32g031xx.s + syscalls.c
    │   └── NodeLib/          # RS485 v2 protocol (Node/NodeMaster/Id/Message/Endpoint/Operation) + ConfigStore
    └── Modules/
        ├── Bootloader/       # bus-resident OTA bootloader — one binary for all boards (base skeleton)
        ├── MainController/   # RS485 bus master (base skeleton)
        └── TemperatureNode/  # duct temperature slave node — framework in place (DS18B20 1-Wire driver, DuctChannel/TemperatureHandler)
```

`ControllerNode/` and `Thermostat/` are not created yet. Full rationale for this layout is in `Spec/Software-Architecture-Spec.md`. `Bootloader`, `MainController` and `TemperatureNode` build today (`make -C Software build`, or the `Software/` CMake project directly); `ControllerNode` and `Thermostat` don't exist yet. The message model (`channel`→`endpoint`, redesigned `operation`) is `Spec/Node-Message-Model-Spec.md`.

## System architecture

- **Main RS485 bus:** `MainController` (node ID `0`, bus master) plus `ControllerNode` and `TemperatureNode` as slaves (IDs `1..N`), all speaking the `NodeLib` v2 protocol — see `Spec/RS485-Node-Protocol-Spec-STM32G030.md` for the wire format (variable-length frame, CRC16, hardware USART DE) and `Spec/Node-Bus-Hardware-Design-Spec.md` / `Spec/Node-Bus-Power-Path-Spec.md` for the shared 48V RJ45 physical layer.
- **Thermostat link:** each `ControllerNode` talks to its own `Thermostat` over a *separate, dedicated point-to-point link*, not the main bus — `Thermostat` is never addressable from `MainController`. Confirmed: this link reuses `NodeLib`'s `Id`/`Message`/CRC framing but **not** the main bus's round-robin polling/discovery state machine (`NodeMaster`) — no arbitration is needed between two fixed endpoints, so `NodeLib` should be split into a standalone framing layer plus the bus-master layer built on it. See `Spec/ControllerNode-Thermostat-Link-Spec.md`.
- **Building server (`Webserver/`):** standalone Go binary — the `MainController` connects out to it over one LAN TCP socket (`Spec/MainController-Server-Link-Spec.md`), it decodes the NodeLib frame stream into SQLite and serves the operator SPA (HTTP + WebSocket). LAN-only, trusted network, no TLS, single 16-byte uplink token, no UI/API auth by design. **`Webserver/internal/nodelib` is a hand-maintained Go re-implementation of `Software/Lib/NodeLib` — any wire-format change (frame layout, CRC, `Endpoint`/`Operation` enums, value encodings) must be made in both, kept byte-compatible.** Deploy notes in `Webserver/README.md`; no CI for `Webserver/` yet.
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
- **Build system:** CMake — `add_stm32_executable()` (`Software/cmake/stm32.cmake`) builds each `Modules/*` into a `.elf` (+ `.bin`/`.hex`/size + `flash-<name>`), top-level `Software/CMakeLists.txt` aggregates `Lib/*` and `Modules/*`. Toolchain `Software/cmake/arm-none-eabi-cortex-m0plus.cmake` (`-mcpu=cortex-m0plus -mthumb`, newlib-nano). Shared startup + per-module `.ld` are in place. `make -C Software build`.
- **Driver layer:** STM32Cube HAL/LL, wrapped by `Lib/HAL` — protocol and application code never includes ST headers directly.
- **Unit tests:** `Software/test/` is a *separate host-native CMake project* (not part of the ARM build) — the portable logic compiled with the runner's g++ against fakes in `test/fake/` (Hal + `ConfigStore` doubles, `test/fake/include/stm32g0xx_hal.h` dummy) and run under `ctest`. Tiny in-tree xUnit harness in `test/framework/` (`CC_TEST` / `CC_CHECK`); no external deps. Each module keeps its suites next to the code in a `test/` folder, registered with `cc_add_test()`. `make -C Software test`; `make -C Software all` runs `check` + `test` + `build`. See `Software/test/README.md`.
- **MCU:** STM32G031F8P6 (Cortex-M0+, 64 MHz, 64 KB flash / 8 KB SRAM, TSSOP20) — locked in 2026-09-08 for all four boards (MainController, ControllerNode, TemperatureNode, Thermostat). Drop-in replacement for the earlier STM32G030F6P6TR: identical pinout, +32 KB flash (bus-resident DFU bootloader + NINA driver headroom), plus LPUART1 / RTC+backup-registers / TIM2. HAL device define is `STM32G031xx`.
- **Flashing:** SEGGER J-Link — a CMake custom target per module shells out to `JLinkExe` with a generated commander script. **`JLinkExe`/`JLinkGDBServer` are not yet installed on this machine** — install the J-Link Software Pack before the flash target will run.
- **CI:** `.github/workflows/cmake-single-platform.yml` — on push/PR to `main`, three jobs: `format` (`make check`, clang-format pinned to 23.1.0 — matches the pipx install the tree was formatted with), `build` (ARM toolchain → build `Software/` `Release` → upload per-module `.elf`/`.bin`/`.hex`), `test` (`make test` → host `ctest`). Covers `Software/` only, not `Webserver/`.

## Specs index

| Doc | Covers |
|---|---|
| `RS485-Node-Protocol-Spec-STM32G030.md` | Main bus wire protocol (frame format, CRC, framing/resync, buffering) |
| `Node-Flash-Layout-and-Bootloader-Spec.md` | Flash partition map, bus-resident OTA bootloader (speaks NodeLib), app image format, persistent `NodeId`/config store |
| `Node-Message-Model-Spec.md` | Message header rethink: `channel`→`endpoint` (flat named-thing enum in blocks), redesigned `operation` verbs, value encodings |
| `Node-Bus-Hardware-Design-Spec.md` | Main bus physical layer (48V PoE-class power, RJ45 pinout, connector part) + node core schematic (§6: MCU support, transceiver, LEDs, buttons, pin plan) |
| `Node-Bus-Power-Path-Spec.md` | Per-node 48V→5V→3.3V regulation chain |
| `Software-Architecture-Spec.md` | Module map, directory layout, build/toolchain, code style |
| `MainController-Spec.md` | Bus-master role + 48V power input / bus injection (§3); NINA-W152 Wi-Fi connectivity (§5); supervisory bridge role |
| `MainController-Server-Link-Spec.md` | MainController ↔ server link: relay NodeLib frames verbatim over one LAN TCP socket; server decodes + stores to SQLite + serves an SPA (HTTP + WebSocket); `0x60` uplink endpoint block; OTA-over-uplink |
| `TemperatureNode-Spec.md` | Duct temperature sensing node; sensor choice still open |
| `ControllerNode-Thermostat-Link-Spec.md` | Per-room point-to-point link + Thermostat hardware (§4: G030 + I²C OLED + 2 buttons); link physical layer still open |
