set(VENDOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Vendor)

# Shared by both targets below -- HalCore is the same sources minus Uart (see
# its own comment further down for why that split exists).
set(HAL_CORE_SOURCES
    Pin.h
    Gpio.h
    Gpio.cpp
    Adc.h
    Adc.cpp
    OneWire.h
    OneWire.cpp
    I2c.h
    I2c.cpp
    BitBangSerial.h
    BitBangSerial.cpp
    Flash.h
    Flash.cpp
    Crc.h
    Crc.cpp
    Tick.h
    Tick.cpp
    System.h
    System.cpp
    Backup.h
    Backup.cpp

    ${VENDOR_DIR}/CMSIS/Device/ST/STM32G0xx/Source/Templates/system_stm32g0xx.c

    ${VENDOR_DIR}/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal.c
    ${VENDOR_DIR}/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_cortex.c
    ${VENDOR_DIR}/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_crc.c
    ${VENDOR_DIR}/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_crc_ex.c
    ${VENDOR_DIR}/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_gpio.c
    ${VENDOR_DIR}/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_pwr.c
    ${VENDOR_DIR}/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_pwr_ex.c
    ${VENDOR_DIR}/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_rcc.c
    ${VENDOR_DIR}/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_rcc_ex.c
)

function(configure_hal_target target_name)
    target_include_directories(${target_name} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${VENDOR_DIR}/CMSIS/Core/Include
        ${VENDOR_DIR}/CMSIS/Device/ST/STM32G0xx/Include
        ${VENDOR_DIR}/STM32G0xx_HAL_Driver/Inc
    )
    target_compile_definitions(${target_name} PUBLIC
        STM32G031xx
        USE_HAL_DRIVER
    )
endfunction()

add_library(Hal STATIC
    ${HAL_CORE_SOURCES}
    Uart.h
    Uart.cpp
    ${VENDOR_DIR}/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_uart.c
    ${VENDOR_DIR}/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_uart_ex.c
)
configure_hal_target(Hal)

# Everything Hal has except Uart -- for the bootloader only (Modules/
# Bootloader/CMakeLists.txt), which hand-rolls its own minimal,
# interrupt-driven USART driver directly on the registers (OtaUart.cpp)
# rather than pulling in Hal::Uart's ~17 KB STM32Cube-backed one. Linking
# against Hal itself doesn't work for this: OtaUart.cpp's own
# USART1_IRQHandler/USART2_IRQHandler would collide with Uart.cpp's
# same-named ones at link time (found on the bench -- with -flto, an
# archive's unreferenced members aren't reliably excluded from symbol
# resolution the way a non-LTO archive link would, so "nothing calls
# Uart::Init() here" isn't enough to make the conflict go away on its own).
# Keeping Uart.cpp out of this target's sources entirely sidesteps the clash
# by construction instead of relying on any weak/strong override trick (tried
# one -- see Uart.cpp's own comment on USART1_IRQHandler for why that broke
# every other module instead).
add_library(HalCore STATIC
    ${HAL_CORE_SOURCES}
)
configure_hal_target(HalCore)
