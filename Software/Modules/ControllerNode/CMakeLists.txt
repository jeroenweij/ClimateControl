add_stm32_executable(controllerNode
    SOURCES
        main.cpp
        ImageInfo.cpp
        Damper.cpp
        ThermostatLink.cpp
        ControllerHandler.cpp
    LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/controllerNode.ld
    MODULE ControllerNode
    LIBRARIES
        Hal
        Board
        Tools
        NodeLib
    BOOTLOADER bootloader
)
