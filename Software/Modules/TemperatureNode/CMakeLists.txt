add_stm32_executable(temperatureNode
    SOURCES
        main.cpp
        ImageInfo.cpp
        TemperatureHandler.cpp
        DuctChannel.cpp
        Ds18b20.cpp
    LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/temperatureNode.ld
    MODULE TemperatureNode
    LIBRARIES
        Hal
        Board
        Tools
        NodeLib
    BOOTLOADER bootloader
)
