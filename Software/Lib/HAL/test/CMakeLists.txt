cc_add_test(CrcTests SOURCES CrcTests.cpp)

# The real 1-Wire driver: its bit-bang I/O pokes the fake GPIO register block
# harmlessly, and OneWire::Crc8 is the point of the test.
cc_add_test(OneWireTests SOURCES OneWireTests.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../OneWire.cpp)
