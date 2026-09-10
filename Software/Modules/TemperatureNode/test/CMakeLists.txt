set(TN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

cc_add_test(Ds18b20Tests
    SOURCES
        Ds18b20Tests.cpp
        FakeOneWire.cpp
        ${TN_DIR}/Ds18b20.cpp
    INCLUDES ${TN_DIR}
)

cc_add_test(DuctChannelTests
    SOURCES
        DuctChannelTests.cpp
        FakeOneWire.cpp
        ${TN_DIR}/Ds18b20.cpp
        ${TN_DIR}/DuctChannel.cpp
        ${TN_DIR}/TemperatureHandler.cpp
    INCLUDES ${TN_DIR}
)
