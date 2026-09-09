add_library(NodeLib STATIC
    EEndpoint.h
    EOperation.h
    EFirmware.h
    INodeHandler.h
    Id.h
    Id.cpp
    ConfigStore.h
    ConfigStore.cpp
    Frame.h
    Frame.cpp
    ErrorHandler.h
    ErrorHandler.cpp
    Node.h
    Node.cpp
    NodeMaster.h
    NodeMaster.cpp
)

target_include_directories(NodeLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(NodeLib PUBLIC Hal Board Tools)
