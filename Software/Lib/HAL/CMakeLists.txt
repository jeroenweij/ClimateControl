set(VENDOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Vendor)

add_library(Hal STATIC
    Pin.h
    Gpio.h
    Gpio.cpp
    Uart.h
    Uart.cpp
    OneWire.h
    OneWire.cpp
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
    ${VENDOR_DIR}/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_uart.c
    ${VENDOR_DIR}/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_uart_ex.c
)

target_include_directories(Hal PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VENDOR_DIR}/CMSIS/Core/Include
    ${VENDOR_DIR}/CMSIS/Device/ST/STM32G0xx/Include
    ${VENDOR_DIR}/STM32G0xx_HAL_Driver/Inc
)

target_compile_definitions(Hal PUBLIC
    STM32G031xx
    USE_HAL_DRIVER
)
