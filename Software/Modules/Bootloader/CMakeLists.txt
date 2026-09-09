set(NODELIB_DIR ${CMAKE_SOURCE_DIR}/Lib/NodeLib)

add_stm32_executable(bootloader
    SOURCES
        main.cpp
        AppImage.cpp
        AppImage.h
        FirmwareSlave.cpp
        FirmwareSlave.h
        OtaUart.cpp
        OtaUart.h
        # ConfigStore + the NodeLib framing layer (Frame/Id/Message) are
        # compiled straight into the bootloader -- built -Os here rather than
        # linked from libNodeLib.a (-O0), and Node/NodeMaster/Logger left out
        # entirely. Node-Flash spec §6.1.
        ${NODELIB_DIR}/ConfigStore.cpp
        ${NODELIB_DIR}/Frame.cpp
        ${NODELIB_DIR}/Id.cpp
    LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/bootloader.ld
    LIBRARIES
        Hal
        Board
        Tools
)

target_include_directories(bootloader PRIVATE ${NODELIB_DIR})

# Hard 8 KB budget -- size-optimise and LTO every TU in the image.
target_compile_options(bootloader PRIVATE -Os -flto)
target_link_options(bootloader PRIVATE -Os -flto)
