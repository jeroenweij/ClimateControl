# Runs a J-Link commander script and fails loudly if it did not complete.
# Invoked at build time by the flash-<name> targets (see stm32.cmake):
#
#   cmake -DJLINK_EXE=<JLinkExe> -DJLINK_SCRIPT=<file.jlink> -DJLINK_WHAT=<label>
#         -P run_jlink.cmake
#
# JLinkExe exits 0 even when it never reached the target unless it is given
# -ExitOnError 1, so without it a flash that failed to connect looks like a
# success. With it, the first failing command (connect, loadfile, ...) ends the
# session with a nonzero exit, and this script turns that into a clear error.
# J-Link's own output streams straight to the terminal, so the reason is on
# screen right above the message.

if(NOT JLINK_EXE)
    message(FATAL_ERROR
        "\n*** FLASH FAILED: ${JLINK_WHAT} -- JLinkExe not found. Install the SEGGER J-Link "
        "Software Pack and re-run CMake configure.\n")
endif()

execute_process(
    COMMAND ${JLINK_EXE} -device STM32G031F8 -if SWD -speed 4000 -autoconnect 1 -NoGui 1
            -ExitOnError 1 -CommanderScript ${JLINK_SCRIPT}
    RESULT_VARIABLE _result)

if(NOT _result EQUAL 0)
    message(FATAL_ERROR
        "\n*** FLASH FAILED: ${JLINK_WHAT} (JLinkExe exit ${_result}) -- the target was NOT "
        "(fully) programmed. See the J-Link output above; \"Cannot connect to target\" means "
        "check the SWD cable, target power and that nothing else holds the probe.\n")
endif()

message(STATUS "Flashed ${JLINK_WHAT} OK")
