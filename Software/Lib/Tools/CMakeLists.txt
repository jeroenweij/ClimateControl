add_library(Tools STATIC
    DelayTimer.h
    DelayTimer.cpp
    SStream.h
    Logger.h
    Logger.cpp
)

target_include_directories(Tools PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(Tools PUBLIC Hal)
