set(NODELIB_DIR ${CMAKE_SOURCE_DIR}/Lib/NodeLib)

add_stm32_executable(mainBootloader
    SOURCES
        main.cpp
        AppImage.cpp
        AppImage.h
        UplinkHandler.h
        UplinkHandler.cpp
        NinaUart.h
        NinaUart.cpp
        Firmware.h
        Firmware.cpp
        # The NINA URC/line classifier, shared with the application's driver
        # (compiled in here, -Os, like the NodeLib files below).
        ${CMAKE_SOURCE_DIR}/Lib/Nina/NinaLineParser.cpp
        ${CMAKE_SOURCE_DIR}/Lib/Nina/NinaAt.cpp
        ${CMAKE_SOURCE_DIR}/Lib/Nina/NinaLink.cpp
        # The NodeLib framing layer (Frame/Id/Message) is compiled straight
        # into mainBootloader -- built -Os here rather than linked from
        # libNodeLib.a (-O0), and ConfigStore/Node/NodeMaster/Logger left out
        # entirely: MainController has no ConfigRecord/NodeId and no bus
        # while resident here (Node-Flash-Layout-and-Bootloader-Spec.md §6.1,
        # §7).
        ${NODELIB_DIR}/Frame.cpp
        ${NODELIB_DIR}/Id.cpp
    LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/mainBootloader.ld
    LIBRARIES
        # HalCoreOs/ToolsOs, not HalCore/Tools -- always -Os regardless of
        # CMAKE_BUILD_TYPE, needed to fit Debug into the 10 KB budget. See
        # HalCoreOs's comment in Lib/HAL/CMakeLists.txt.
        HalCoreOs
        Board
        ToolsOs
)

target_include_directories(mainBootloader PRIVATE ${NODELIB_DIR} ${CMAKE_SOURCE_DIR}/Lib/Nina)

# CC_BOARD_MainController, same define add_stm32_executable's MODULE arg
# would set (Lib/Board/BoardPins.h's NinaCts/NinaRts/NinaReset are
# gated behind it) -- but MODULE itself is not used here: it also finalizes
# an OTA image descriptor and copies a <Module>_<version>.bin for the
# server's firmware repository, neither of which applies to a bootloader
# (SWD-flashed only, never OTA'd -- Node-Flash-Layout-and-Bootloader-Spec.md
# §3).
target_compile_definitions(mainBootloader PRIVATE CC_BOARD_MainController)

# Hard 10 KB budget (mainBootloader.ld) -- size-optimise and LTO every TU.
target_compile_options(mainBootloader PRIVATE -Os -flto)
target_link_options(mainBootloader PRIVATE -Os -flto)
