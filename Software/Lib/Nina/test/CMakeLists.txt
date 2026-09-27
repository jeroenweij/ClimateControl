set(NINA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

cc_add_test(NinaLineParserTests
    SOURCES
        NinaLineParserTests.cpp
        ${NINA_DIR}/NinaLineParser.cpp
    INCLUDES ${NINA_DIR}
)

cc_add_test(NinaLinkQueueTests
    SOURCES
        NinaLinkQueueTests.cpp
        ${NINA_DIR}/NinaLink.cpp
        ${NINA_DIR}/NinaAt.cpp
        ${NINA_DIR}/NinaLineParser.cpp
    INCLUDES ${NINA_DIR}
)
# Board::NinaReset (NinaAt) is behind this guard -- mirror the firmware build.
target_compile_definitions(NinaLinkQueueTests PRIVATE CC_BOARD_MainController)
