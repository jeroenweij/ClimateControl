# Standalone CMake project for the NinaEnable bring-up utility -- deliberately
# NOT added as a subdirectory of Software/CMakeLists.txt, so it never builds
# as part of the normal `make -C Software build` and can't drift into being
# treated as a real Modules/* firmware image. Configure/build it on its own:
#
#   cmake -S . -B build
#   cmake --build build
#   cmake --build build --target flash-ninaEnable   # or: make program
#
# Reuses Lib/HAL, Lib/Board, Lib/Tools, Lib/Startup and cmake/stm32.cmake
# straight from the main tree (same STM32G031 toolchain/build helpers, no
# copies) via relative paths -- this directory must stay a direct sibling of
# Lib/ and cmake/ under Software/.

cmake_minimum_required(VERSION 3.20)

set(CC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

set(CMAKE_TOOLCHAIN_FILE ${CC_ROOT}/cmake/arm-none-eabi-cortex-m0plus.cmake CACHE FILEPATH "")

project(NinaEnable C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

include(${CC_ROOT}/cmake/stm32.cmake)

add_subdirectory(${CC_ROOT}/Lib/Startup ${CMAKE_CURRENT_BINARY_DIR}/Lib/Startup)
add_subdirectory(${CC_ROOT}/Lib/HAL     ${CMAKE_CURRENT_BINARY_DIR}/Lib/HAL)
add_subdirectory(${CC_ROOT}/Lib/Board   ${CMAKE_CURRENT_BINARY_DIR}/Lib/Board)
add_subdirectory(${CC_ROOT}/Lib/Tools   ${CMAKE_CURRENT_BINARY_DIR}/Lib/Tools)

add_stm32_executable(ninaEnable
    SOURCES
        main.cpp
    LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/ninaEnable.ld
    LIBRARIES
        Hal
        Board
        Tools
)

# BoardPins.h gates the NINA/MainController pins behind this define (normally
# set by add_stm32_executable's MODULE argument, which also wires up OTA
# image-descriptor versioning this bring-up image has no use for).
target_compile_definitions(ninaEnable PRIVATE CC_BOARD_MainController)
