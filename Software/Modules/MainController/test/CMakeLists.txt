set(MC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

cc_add_test(NinaLineParserTests
    SOURCES
        NinaLineParserTests.cpp
        ${MC_DIR}/NinaLineParser.cpp
    INCLUDES ${MC_DIR}
)

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
        ${MC_DIR}/NinaAt.cpp
        ${MC_DIR}/NinaLineParser.cpp
    INCLUDES ${MC_DIR}
)
# Board::NinaReset/NinaRts/Usart2Tx/Usart2Rx (NinaAt.cpp) are behind this
# guard, set by add_stm32_executable() for the real firmware build -- mirror
# it here (same rationale as ControllerNode's CC_BOARD_ControllerNode).
target_compile_definitions(UplinkHandlerTests PRIVATE CC_BOARD_MainController)
