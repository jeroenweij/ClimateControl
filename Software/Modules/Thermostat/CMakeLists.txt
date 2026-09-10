add_stm32_executable(thermostat
    SOURCES
        main.cpp
        ImageInfo.cpp
        ThermostatHandler.cpp
    LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/thermostat.ld
    MODULE Thermostat
    LIBRARIES
        Hal
        Board
        Tools
        NodeLib
    BOOTLOADER bootloader
)
