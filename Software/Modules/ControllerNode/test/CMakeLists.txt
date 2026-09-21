set(CN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

cc_add_test(RoomControlLoopTests
    SOURCES
        RoomControlLoopTests.cpp
        ${CN_DIR}/Damper.cpp
        ${CN_DIR}/ThermostatLink.cpp
        ${CN_DIR}/SupplyTemp.cpp
        ${CN_DIR}/RoomControlLoop.cpp
    INCLUDES ${CN_DIR}
)
# Board::ServoEnable/ServoCurrentSenseChannel (Damper.cpp) are behind this
# guard, set by add_stm32_executable() for the real firmware build
# (cmake/stm32.cmake) -- mirror it here so BoardPins.h exposes the same names.
target_compile_definitions(RoomControlLoopTests PRIVATE CC_BOARD_ControllerNode)

cc_add_test(ControllerHandlerTests
    SOURCES
        ControllerHandlerTests.cpp
        ${CN_DIR}/Damper.cpp
        ${CN_DIR}/ThermostatLink.cpp
        ${CN_DIR}/SupplyTemp.cpp
        ${CN_DIR}/RoomControlLoop.cpp
        ${CN_DIR}/ControllerHandler.cpp
    INCLUDES ${CN_DIR}
)
target_compile_definitions(ControllerHandlerTests PRIVATE CC_BOARD_ControllerNode)

cc_add_test(ThermostatLinkTests
    SOURCES
        ThermostatLinkTests.cpp
        ${CN_DIR}/Damper.cpp
        ${CN_DIR}/ThermostatLink.cpp
    INCLUDES ${CN_DIR}
)
target_compile_definitions(ThermostatLinkTests PRIVATE CC_BOARD_ControllerNode)

cc_add_test(DamperTests
    SOURCES
        DamperTests.cpp
        ${CN_DIR}/Damper.cpp
    INCLUDES ${CN_DIR}
)
target_compile_definitions(DamperTests PRIVATE CC_BOARD_ControllerNode)
