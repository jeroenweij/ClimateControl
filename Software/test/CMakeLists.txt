# Host (native) unit-test build -- entirely separate from the ARM firmware
# build one level up. The firmware is cross-compiled for Cortex-M0+ and cannot
# run on a CI runner, so the portable logic (framing, CRC, the bus-master state
# machine, sensor decode) is compiled for the host against the fakes in fake/
# and exercised with ctest.
#
#   cmake -S Software/test -B Software/build-test
#   cmake --build Software/build-test
#   ctest --test-dir Software/build-test --output-on-failure
#
# or just `make -C Software test`.

cmake_minimum_required(VERSION 3.20)
project(ClimateControlTests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Match the language subset the firmware is built with.
add_compile_options(-Wall -Wextra -fno-exceptions -fno-rtti -g -O1)

enable_testing()

set(CC_SW_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# --- host doubles for Lib/HAL + NodeLib::ConfigStore -----------------------
add_library(test_fakes STATIC
    fake/FakeClock.cpp
    fake/FakeBus.cpp
    fake/FakeCrc.cpp
    fake/FakeGpio.cpp
    fake/FakeSystem.cpp
    fake/FakeBackup.cpp
    fake/FakeConfigStore.cpp
    fake/FakeImageDescriptor.cpp
)
target_include_directories(test_fakes PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/fake
    ${CMAKE_CURRENT_SOURCE_DIR}/fake/include
    ${CC_SW_ROOT}/Lib/HAL
    ${CC_SW_ROOT}/Lib/Tools
    ${CC_SW_ROOT}/Lib/Board
    ${CC_SW_ROOT}/Lib/NodeLib
)

# --- the portable production code, built for the host ---------------------
add_library(nodelib_host STATIC
    ${CC_SW_ROOT}/Lib/Tools/DelayTimer.cpp
    ${CC_SW_ROOT}/Lib/Tools/Logger.cpp
    ${CC_SW_ROOT}/Lib/NodeLib/Id.cpp
    ${CC_SW_ROOT}/Lib/NodeLib/Frame.cpp
    ${CC_SW_ROOT}/Lib/NodeLib/ErrorHandler.cpp
    ${CC_SW_ROOT}/Lib/NodeLib/Node.cpp
    ${CC_SW_ROOT}/Lib/NodeLib/NodeMaster.cpp
)
target_link_libraries(nodelib_host PUBLIC test_fakes)

# --- the harness (main() + bus helpers), compiled into every test exe -----
add_library(test_framework INTERFACE)
target_sources(test_framework INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/framework/Test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/framework/BusHelpers.cpp
)
target_include_directories(test_framework INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/framework)
target_link_libraries(test_framework INTERFACE nodelib_host)

# cc_add_test(<name> SOURCES a.cpp ...  [SOURCES_RAW extra.cpp ...] [INCLUDES ...])
function(cc_add_test name)
    cmake_parse_arguments(T "" "" "SOURCES;INCLUDES" ${ARGN})
    add_executable(${name} ${T_SOURCES})
    target_link_libraries(${name} PRIVATE test_framework)
    if(T_INCLUDES)
        target_include_directories(${name} PRIVATE ${T_INCLUDES})
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_subdirectory(${CC_SW_ROOT}/Lib/Tools/test               ${CMAKE_BINARY_DIR}/Tools_test)
add_subdirectory(${CC_SW_ROOT}/Lib/HAL/test                 ${CMAKE_BINARY_DIR}/HAL_test)
add_subdirectory(${CC_SW_ROOT}/Lib/NodeLib/test             ${CMAKE_BINARY_DIR}/NodeLib_test)
add_subdirectory(${CC_SW_ROOT}/Modules/TemperatureNode/test ${CMAKE_BINARY_DIR}/TemperatureNode_test)
