add_stm32_executable(mainController
    SOURCES
        main.cpp
        ImageInfo.cpp
        NinaLineParser.cpp
        NinaAt.cpp
        BudgetAllocator.cpp
        UplinkHandler.cpp
        DebugLog.cpp
    LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/mainController.ld
    MODULE MainController
    LIBRARIES
        Hal
        Board
        Tools
        NodeLib
    BOOTLOADER bootloader
)
