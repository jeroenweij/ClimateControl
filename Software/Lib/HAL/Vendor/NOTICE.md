# Vendored third-party sources

Files under this directory are vendored (copied, not submoduled) from ST/ARM upstream repositories.
They are **not** hand-written project code — do not run `.clang-format` over them, and do not
modify them beyond what's strictly necessary to build (if a change is ever needed, note it inline
and explain why, so it's obvious this diverges from upstream).

Fetched 2026-09-07. Only the specific files this project's `Hal` library needs were pulled — not
full clones. Re-run the fetch commands below (adjusting the tag) to update.

## CMSIS Core (ARM)

- **Source:** https://github.com/STMicroelectronics/STM32CubeG0 (`Drivers/CMSIS/Include/`)
- **Tag:** `v1.6.3` — commit `878a6fd8ad440c70ee32bc6d7ad526babb6e04f4`
- **License:** Apache License 2.0 — Copyright (c) 2009-2018 ARM Limited
- **Files:** `CMSIS/Core/Include/{core_cm0plus.h, cmsis_gcc.h, cmsis_compiler.h, cmsis_version.h, mpu_armv7.h}`
  (`mpu_armv7.h` added 2026-09-07, found via a real build error -- `core_cm0plus.h` includes it unconditionally)

## CMSIS Device (STM32G0xx)

- **Source:** https://github.com/STMicroelectronics/cmsis_device_g0
- **Tag:** `v1.4.5` — commit `1211f427f1428edf496ba47b5f5b3a448fbd6a53`
- **License:** Apache License 2.0 — ARM Limited / STMicroelectronics
- **Files:**
  - `CMSIS/Device/ST/STM32G0xx/Include/{stm32g030xx.h, stm32g0xx.h, system_stm32g0xx.h}`
  - `CMSIS/Device/ST/STM32G0xx/Source/Templates/system_stm32g0xx.c`

## STM32G0xx HAL Driver

- **Source:** https://github.com/STMicroelectronics/stm32g0xx_hal_driver
- **Tag:** `v1.4.7` — commit `46127071c32579d4c872053952882c32705bc054`
- **License:** BSD-3-Clause — STMicroelectronics
- **Modules vendored** (only what `Hal::Gpio`/`Hal::Uart`/`Hal::Crc`/`Hal::Tick` need — RCC/CORTEX
  are required by `HAL_Init()` and clock/tick bring-up even though nothing in `Hal` calls them
  directly beyond init):
  - `STM32G0xx_HAL_Driver/Inc/` and `Src/`: `stm32g0xx_hal`, `stm32g0xx_hal_cortex`,
    `stm32g0xx_hal_crc` (+`_ex`), `stm32g0xx_hal_gpio`, `stm32g0xx_hal_rcc` (+`_ex`),
    `stm32g0xx_hal_uart` (+`_ex`), `stm32g0xx_hal_pwr` (added 2026-09-07 — `stm32g0xx_hal_rcc_ex.c`'s
    LSCO clock-output function calls `HAL_PWR_EnableBkUpAccess`/`DisableBkUpAccess` unconditionally,
    so `HAL_PWR_MODULE_ENABLED` must be defined in `stm32g0xx_hal_conf.h` and this module vendored,
    even though nothing in `Hal` calls PWR functions directly). Its `_ex` companion
    (`stm32g0xx_hal_pwr_ex.h`/`.c`, added 2026-09-07 via build error, same pattern as
    every other module's `_ex` pair) is required unconditionally by `stm32g0xx_hal_pwr.h`.
  - `STM32G0xx_HAL_Driver/Inc/stm32g0xx_hal_def.h` (no `.c`, header-only)
  - `STM32G0xx_HAL_Driver/Inc/Legacy/stm32_hal_legacy.h` (added 2026-09-07, found via a
    real build error -- `stm32g0xx_hal_def.h` includes it unconditionally)
  - `STM32G0xx_HAL_Driver/Inc/stm32g0xx_hal_gpio_ex.h` (added 2026-09-07, found via a
    real build error -- `stm32g0xx_hal_gpio.h` includes it unconditionally)
  - `STM32G0xx_HAL_Driver/Inc/stm32g0xx_ll_rcc.h` (added 2026-09-07 — `stm32g0xx_hal_rcc.h` includes
    this unconditionally, not behind any module guard; it only pulls in `stm32g0xx.h`, already
    vendored, no further cascade)
  - `STM32G0xx_HAL_Driver/Inc/{stm32g0xx_hal_dma.h, stm32g0xx_hal_dma_ex.h, stm32g0xx_ll_dma.h,
    stm32g0xx_ll_dmamux.h}` (added 2026-09-07, found via build error -- `stm32g0xx_hal_uart.h`'s
    handle struct unconditionally references `DMA_HandleTypeDef*` even though nothing here does
    DMA transfers; header-only, no `.c`, since we never call the DMA functions, only need the
    type declared)
  - `STM32G0xx_HAL_Driver/Inc/{stm32g0xx_hal_flash.h, stm32g0xx_hal_flash_ex.h}` (added
    2026-09-07, found via build error -- `stm32g0xx_hal_rcc.c`'s clock-config path
    unconditionally uses the `__HAL_FLASH_*_LATENCY` macros; header-only, no `.c`, since
    nothing here calls the FLASH module's actual functions, only its macros)
  - `STM32G0xx_HAL_Driver/Inc/stm32g0xx_hal_conf_template.h` — copied to `../stm32g0xx_hal_conf.h`
    (hand-trimmed to enable only the modules above; that trimmed copy lives outside `Vendor/` since
    it's a project-specific config file, not verbatim upstream source)

**How the two 2026-09-07 additions were found:** systematically grepping every vendored file's
`#include` directives for anything not yet vendored, distinguishing genuinely unconditional
`#include`s (structural dependencies) from the ones inside `#ifdef HAL_XXX_MODULE_ENABLED` guards in
`stm32g0xx_hal_conf_template.h` (that huge guarded list — ADC, TIM, SPI, RTC, etc. — is inert as long
as our own `stm32g0xx_hal_conf.h` never defines those macros). Only `stm32g0xx_ll_rcc.h` and
`stm32g0xx_hal_pwr.h` turned out to be real gaps.

## Note on STM32CubeG0's structure

`STM32CubeG0` itself is a superproject with the HAL driver and CMSIS device files as **git
submodules** (along with ~20 more submodules for BSPs/middleware this project doesn't use) — that's
why they're fetched from their own standalone repos above rather than from paths inside
`STM32CubeG0` directly. Only the CMSIS **core** files came from the superproject itself (those
aren't submoduled there). Cloning `STM32CubeG0` with submodules would pull in all of that unrelated
BSP/middleware content for no benefit here — avoided deliberately.
