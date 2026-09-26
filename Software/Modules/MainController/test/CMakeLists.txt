set(MC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

cc_add_test(BudgetAllocatorTests
    SOURCES
        BudgetAllocatorTests.cpp
        ${MC_DIR}/BudgetAllocator.cpp
    INCLUDES ${MC_DIR}
)

cc_add_test(UplinkHandlerTests
    SOURCES
        UplinkHandlerTests.cpp
        ${MC_DIR}/BudgetAllocator.cpp
        ${MC_DIR}/UplinkHandler.cpp
        ${MC_DIR}/ThermostatMonitor.cpp
        ${MC_DIR}/HalNinaPort.cpp
        ${CC_SW_ROOT}/Lib/Nina/NinaAt.cpp
        ${CC_SW_ROOT}/Lib/Nina/NinaLink.cpp
        ${CC_SW_ROOT}/Lib/Nina/NinaLineParser.cpp
    INCLUDES ${MC_DIR} ${CC_SW_ROOT}/Lib/Nina
)
# Board::NinaReset/NinaRts/Usart2Tx/Usart2Rx (NinaAt/HalNinaPort) are behind this
# guard, set by add_stm32_executable() for the real firmware build -- mirror
# it here (same rationale as ControllerNode's CC_BOARD_ControllerNode).
target_compile_definitions(UplinkHandlerTests PRIVATE CC_BOARD_MainController CC_NINA_LOG)

cc_add_test(NinaBringUpTests
    SOURCES
        NinaBringUpTests.cpp
        ${MC_DIR}/BudgetAllocator.cpp
        ${MC_DIR}/UplinkHandler.cpp
        ${MC_DIR}/ThermostatMonitor.cpp
        ${MC_DIR}/HalNinaPort.cpp
        ${CC_SW_ROOT}/Lib/Nina/NinaAt.cpp
        ${CC_SW_ROOT}/Lib/Nina/NinaLink.cpp
        ${CC_SW_ROOT}/Lib/Nina/NinaLineParser.cpp
    INCLUDES ${MC_DIR} ${CC_SW_ROOT}/Lib/Nina ${CC_SW_ROOT}/test/scenarios
)
target_compile_definitions(NinaBringUpTests PRIVATE CC_BOARD_MainController CC_NINA_LOG)

cc_add_test(ThermostatMonitorTests
    SOURCES
        ThermostatMonitorTests.cpp
        ${MC_DIR}/ThermostatMonitor.cpp
    INCLUDES ${MC_DIR}
)
