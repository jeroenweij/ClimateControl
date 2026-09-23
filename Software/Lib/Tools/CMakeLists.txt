add_library(Tools STATIC
    DelayTimer.h
    DelayTimer.cpp
    SStream.h
    Logger.h
    Logger.cpp
    LogRing.h
    LogRing.cpp
    BootHealth.h
    BootHealth.cpp
)

target_include_directories(Tools PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(Tools PUBLIC HalCore)

# Same sources as Tools, but always -Os and linked against HalCoreOs instead
# of HalCore -- for Modules/MainBootloader only. See HalCoreOs's comment in
# Lib/HAL/CMakeLists.txt for why; must stay paired with HalCoreOs; linking a
# module against one of {HalCore, Tools} and the other of {HalCoreOs,
# ToolsOs} would pull both HalCore and HalCoreOs into the same link and
# duplicate-symbol on every Hal:: class.
add_library(ToolsOs STATIC
    DelayTimer.h
    DelayTimer.cpp
    SStream.h
    Logger.h
    Logger.cpp
    LogRing.h
    LogRing.cpp
    BootHealth.h
    BootHealth.cpp
)

target_include_directories(ToolsOs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(ToolsOs PUBLIC HalCoreOs)
target_compile_options(ToolsOs PRIVATE -Os)
