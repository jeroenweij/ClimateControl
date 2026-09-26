# Emits a J-Link commander script that flashes JLINK_HEX and resets.
# Invoked at build time by the flash-<name> targets (see stm32.cmake).
#
# Between programming and the final reset it also zeroes TAMP->BKP0R (the
# enter-bootloader request) and TAMP->BKP1R (Tools::BootHealth's boot-fail
# counter). Both live in the backup domain, which survives a system reset and
# a reflash -- only a power cycle clears it -- so resets from earlier flashing
# or debugging would otherwise carry over into the new image and can leave the
# bootloader resident. Writing them needs the PWR + RTCAPB bus clocks
# (RCC_APBENR1 PWREN | RTCAPBEN) and PWR_CR1.DBP; PWR_CR1 is written as its
# reset value (0x208) plus DBP, which is exact right after the `r`. The final
# `r` puts RCC/PWR back to reset state.
file(WRITE ${JLINK_OUT}
"si SWD
speed 4000
device STM32G031F8
connect
halt
loadfile ${JLINK_HEX}
r
w4 0x4002103C 0x10000400
w4 0x40007000 0x00000308
w4 0x4000B100 0
w4 0x4000B104 0
r
g
qc
")
