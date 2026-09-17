# NinaEnable

Standalone bring-up utility for the MainController board. It is **not** part
of the normal firmware build (see `Software/CMakeLists.txt`, which never
`add_subdirectory`s this folder) -- it's a throwaway image you flash instead
of the real firmware to get bench access to the on-board NINA-W152 module.

It does exactly two things on boot:

1. Releases `NinaReset` (net `RESET_NINA`) so the NINA leaves reset and runs
   its normal u-connectXpress AT firmware.
2. Blinks the activity LED as a heartbeat.

It never touches `Usart2Tx`/`Usart2Rx`/`NinaCts`/`NinaRts` (USART2) -- those
pins are left at their GPIO reset state (Hi-Z), so wire a USB-TTL adapter
directly to the NINA's own UART pins and talk to it at 115200 8N1 without the
STM32 driving or listening on the line.

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
