# Emits a J-Link commander script that flashes JLINK_HEX and resets.
# Invoked at build time by the flash-<name> targets (see stm32.cmake).
file(WRITE ${JLINK_OUT}
"si SWD
speed 4000
device STM32G031F8
connect
halt
loadfile ${JLINK_HEX}
r
g
qc
")
