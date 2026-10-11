# Thermostat PC simulator (ThermostatSim.cpp) -- not a test: an interactive
# terminal program that runs the real ThermostatHandler/Ssd1306 code against
# the host fakes plus SimI2c, for UI work without hardware. Built alongside
# the unit tests so it can't silently rot; run it with
# `make -C Software sim-thermostat`.
set(TH_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(ThermostatSim
    ThermostatSim.cpp
    SimI2c.cpp
    ${CC_SW_ROOT}/test/framework/BusHelpers.cpp
    ${TH_DIR}/Cht40.cpp
    ${TH_DIR}/Ssd1306.cpp
    ${TH_DIR}/ThermostatHandler.cpp
)
target_include_directories(ThermostatSim PRIVATE ${TH_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${CC_SW_ROOT}/test/framework)
target_link_libraries(ThermostatSim PRIVATE nodelib_host)
# Same board guard as ThermostatHandlerTests (Modules/Thermostat/test).
target_compile_definitions(ThermostatSim PRIVATE CC_BOARD_Thermostat)
