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
        DebugLog.cpp
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
