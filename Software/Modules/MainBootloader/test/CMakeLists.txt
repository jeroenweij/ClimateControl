set(BL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

cc_add_test(FirmwareTests
    SOURCES
        FirmwareTests.cpp
        ${BL_DIR}/Firmware.cpp
    INCLUDES ${BL_DIR}
)

cc_add_test(BootNinaBringUpTests
    SOURCES
        NinaBringUpTests.cpp
        FakeNinaUart.cpp
        ${BL_DIR}/Firmware.cpp
        ${BL_DIR}/UplinkHandler.cpp
        ${CC_SW_ROOT}/Lib/Nina/NinaAt.cpp
        ${CC_SW_ROOT}/Lib/Nina/NinaLink.cpp
        ${CC_SW_ROOT}/Lib/Nina/NinaLineParser.cpp
    INCLUDES ${BL_DIR} ${CC_SW_ROOT}/Lib/Nina ${CMAKE_CURRENT_SOURCE_DIR} ${CC_SW_ROOT}/test/scenarios
)
target_compile_definitions(BootNinaBringUpTests PRIVATE CC_BOARD_MainController)
