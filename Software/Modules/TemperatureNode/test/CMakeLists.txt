set(TN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Both suites touch Board::OneWire1/OneWire2, which BoardPins.h now compiles
# out unless CC_BOARD_TemperatureNode is defined (cmake/stm32.cmake sets this
# automatically for the real firmware target via MODULE; cc_add_test doesn't,
# so it's added explicitly here).
cc_add_test(Ds18b20Tests
    SOURCES
        Ds18b20Tests.cpp
        FakeOneWire.cpp
        ${TN_DIR}/Ds18b20.cpp
    INCLUDES ${TN_DIR}
)
target_compile_definitions(Ds18b20Tests PRIVATE CC_BOARD_TemperatureNode)

cc_add_test(DuctChannelTests
    SOURCES
        DuctChannelTests.cpp
        FakeOneWire.cpp
        ${TN_DIR}/Ds18b20.cpp
        ${TN_DIR}/DuctChannel.cpp
        ${TN_DIR}/TemperatureHandler.cpp
    INCLUDES ${TN_DIR}
)
target_compile_definitions(DuctChannelTests PRIVATE CC_BOARD_TemperatureNode)
