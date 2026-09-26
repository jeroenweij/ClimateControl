add_stm32_executable(mainController
    SOURCES
        main.cpp
        ImageInfo.cpp
        HalNinaPort.cpp
        ${CMAKE_SOURCE_DIR}/Lib/Nina/NinaLineParser.cpp
        ${CMAKE_SOURCE_DIR}/Lib/Nina/NinaAt.cpp
        ${CMAKE_SOURCE_DIR}/Lib/Nina/NinaLink.cpp
        BudgetAllocator.cpp
        UplinkHandler.cpp
        ThermostatMonitor.cpp
    LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/mainController.ld
    MODULE MainController
    LIBRARIES
        Hal
        Board
        Tools
        NodeLib
    BOOTLOADER mainBootloader
)

target_include_directories(mainController PRIVATE ${CMAKE_SOURCE_DIR}/Lib/Nina)

# NinaLink logs its progress through Tools::Logger in the application.
target_compile_definitions(mainController PRIVATE CC_NINA_LOG)

# Bench console: Tools::Logger lines to SEGGER RTT up-channel 0 over the
# J-Link (RttLog.cpp). Off, Logger::Write stays the weak no-op and the log
# reaches the operator only as MainLog over the uplink
# (MainController-Server-Link-Spec.md §5.1).
option(CC_MC_LOG_RTT "MainController: write the log to SEGGER RTT" OFF)
if(CC_MC_LOG_RTT)
    target_sources(mainController PRIVATE RttLog.cpp)
endif()
