add_stm32_executable(mainController
    SOURCES
        main.cpp
        ImageInfo.cpp
    LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/mainController.ld
    LIBRARIES
        Hal
        Board
        Tools
        NodeLib
    BOOTLOADER bootloader
)
