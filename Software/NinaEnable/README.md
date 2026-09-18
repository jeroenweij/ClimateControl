# NinaEnable

Standalone bring-up utility for the MainController board. It is **not** part
of the normal firmware build (see `Software/CMakeLists.txt`, which never
`add_subdirectory`s this folder) -- it's a throwaway image you flash instead
of the real firmware to get bench access to the on-board NINA-W152 module.

It does exactly three things on boot:

1. Releases `NinaReset` (net `RESET_NINA`) so the NINA leaves reset and runs
   its normal u-connectXpress AT firmware.
2. Drives `NinaRts` (STM32 `PA1` -> NINA `UART_CTS`, module pin 21) low.
   u-connectXpress ships with 4-wire HW flow control on by default
   (`MainController-Spec.md` Sec5) and won't transmit unless its `UART_CTS`
   input is asserted; a bench USB-TTL adapter only ever breaks out
   TXD/RXD/GND (header H1), so nothing else drives this line for it.
3. Blinks the activity LED as a heartbeat.

It never touches `Usart2Tx`/`Usart2Rx`/`NinaCts` -- those pins are left at
their GPIO reset state (Hi-Z), so wire a USB-TTL adapter directly to the
NINA's own TXD/RXD pins and talk to it at 115200 8N1 without the STM32
driving or listening on the data lines themselves.

Flashing this **overwrites whatever is currently on the chip** (bootloader
included) over SWD. The linker script stops short of the Config A/B pages at
`0x0800F000` (`Lib/Board/MemoryMap.h`), so the unit's provisioned NodeId
survives -- re-flash the real bootloader+app afterwards (`make -C Software
flash-full MODULE=mainController`) to restore normal operation.

## Build & flash

```
make build     # configure + compile build/ninaEnable.elf/.bin/.hex
make program    # build, then flash over SWD via JLinkExe
```

`JLinkExe` must be on `PATH` (see `Software-Architecture-Spec.md` §3).
