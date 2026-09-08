add_stm32_executable(bootloader
    SOURCES
        main.cpp
        AppImage.cpp
        AppImage.h
    LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/bootloader.ld
    LIBRARIES
        Hal
        Board
        Tools
        # Only ConfigStore.cpp is pulled from NodeLib (--gc-sections drops the
        # rest); the full framing/bus split is still TODO -- Node-Flash spec §8.7.
        NodeLib
)
