# Every Thermostat shares one fixed identity instead of a per-unit provisioned
# one, so any unit is a drop-in spare. The id is NodeLib::THERMOSTAT_NODE_ID,
# read from Id.h so the firmware and the baked-in ConfigRecord can't drift.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/../../Lib/NodeLib/Id.h _thermostat_id_line REGEX "THERMOSTAT_NODE_ID = [0-9]+")
if(NOT _thermostat_id_line MATCHES "THERMOSTAT_NODE_ID = ([0-9]+)")
    message(FATAL_ERROR "Could not read THERMOSTAT_NODE_ID from Lib/NodeLib/Id.h")
endif()
set(THERMOSTAT_NODE_ID ${CMAKE_MATCH_1})

add_stm32_executable(thermostat
    SOURCES
        main.cpp
        ImageInfo.cpp
        ThermostatHandler.cpp
        Cht40.cpp
        Ssd1306.cpp
    LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/thermostat.ld
    MODULE Thermostat
    LIBRARIES
        Hal
        Board
        Tools
        NodeLib
    BOOTLOADER bootloader
    FIXED_NODE_ID ${THERMOSTAT_NODE_ID}
)
