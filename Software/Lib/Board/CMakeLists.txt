add_library(Board INTERFACE)

target_include_directories(Board INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(Board INTERFACE Hal)
