set(NINA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

cc_add_test(NinaLineParserTests
    SOURCES
        NinaLineParserTests.cpp
        ${NINA_DIR}/NinaLineParser.cpp
    INCLUDES ${NINA_DIR}
)
