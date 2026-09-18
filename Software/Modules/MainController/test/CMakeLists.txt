set(MC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

cc_add_test(NinaLineParserTests
    SOURCES
        NinaLineParserTests.cpp
        ${MC_DIR}/NinaLineParser.cpp
    INCLUDES ${MC_DIR}
)
