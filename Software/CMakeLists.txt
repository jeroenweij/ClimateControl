cmake_minimum_required(VERSION 3.20)

set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/arm-none-eabi-cortex-m0plus.cmake CACHE FILEPATH "")

project(ClimateControl C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Firmware version baked into every module's image descriptor (major.minor) and
# used to name the OTA image the build produces
# (<Module>_<version>.bin, e.g. ControllerNode_1.0.bin -- the name the server's
# firmware repository parses). Bump this for a release.
set(CC_FW_VERSION "1.0" CACHE STRING "Firmware version <major>.<minor>")

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/stm32.cmake)

add_subdirectory(Lib)
add_subdirectory(Modules)
