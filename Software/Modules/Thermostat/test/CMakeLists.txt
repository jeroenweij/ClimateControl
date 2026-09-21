set(TH_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

cc_add_test(Cht40Tests
    SOURCES
        Cht40Tests.cpp
        FakeI2c.cpp
        ${TH_DIR}/Cht40.cpp
    INCLUDES ${TH_DIR}
)

cc_add_test(Ssd1306Tests
    SOURCES
        Ssd1306Tests.cpp
        FakeI2c.cpp
        ${TH_DIR}/Ssd1306.cpp
    INCLUDES ${TH_DIR}
)

# Board::UserButton2/I2cSda/I2cScl/OledReset/OledPowerEnable are behind this
# guard (cmake/stm32.cmake sets it automatically for the real firmware target
# via MODULE; cc_add_test doesn't, so it's added explicitly here -- same
# rationale as TemperatureNode's CC_BOARD_TemperatureNode).
cc_add_test(ThermostatHandlerTests
    SOURCES
        ThermostatHandlerTests.cpp
        FakeI2c.cpp
        ${TH_DIR}/Cht40.cpp
        ${TH_DIR}/Ssd1306.cpp
        ${TH_DIR}/ThermostatHandler.cpp
    INCLUDES ${TH_DIR}
)
target_compile_definitions(ThermostatHandlerTests PRIVATE CC_BOARD_Thermostat)
