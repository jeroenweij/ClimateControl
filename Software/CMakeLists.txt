cmake_minimum_required(VERSION 3.20)

set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/arm-none-eabi-cortex-m0plus.cmake CACHE FILEPATH "")

project(ClimateControl C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/stm32.cmake)

add_subdirectory(Lib)
add_subdirectory(Modules)
