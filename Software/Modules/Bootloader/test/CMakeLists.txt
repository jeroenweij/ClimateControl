set(BL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

cc_add_test(FirmwareSlaveTests
    SOURCES
        FirmwareSlaveTests.cpp
        ${BL_DIR}/FirmwareSlave.cpp
    INCLUDES ${BL_DIR}
)
