set(BL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

cc_add_test(FirmwareTests
    SOURCES
        FirmwareTests.cpp
        ${BL_DIR}/Firmware.cpp
    INCLUDES ${BL_DIR}
)
