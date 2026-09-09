# Helpers for building a flashable STM32G031 firmware image from a module.
#
#   add_stm32_executable(<name>
#       SOURCES        a.cpp b.cpp ...
#       LINKER_SCRIPT  ${CMAKE_CURRENT_SOURCE_DIR}/foo.ld
#       LIBRARIES      Hal NodeLib ...
#       BOOTLOADER     bootloader)          # optional
#
# Produces <name>.elf plus <name>.bin / <name>.hex / <name>.map next to it,
# prints the size, and adds a `flash-<name>` target that drives JLinkExe.
#
# With BOOTLOADER <tgt>, also emits <name>-full.hex -- that bootloader's hex
# merged with this app's hex, for one-shot factory programming over SWD -- plus
# a `flash-<name>-full` target. (The per-unit config page with the NodeId is
# still written separately.)

find_program(ARM_OBJCOPY arm-none-eabi-objcopy REQUIRED)
find_program(ARM_SIZE    arm-none-eabi-size    REQUIRED)
find_program(JLINK_EXE   JLinkExe)   # may be absent -- see Software-Architecture-Spec.md §3

set(_STM32_CMAKE_DIR ${CMAKE_CURRENT_LIST_DIR})

function(add_stm32_executable NAME)
    cmake_parse_arguments(ARG "" "LINKER_SCRIPT;BOOTLOADER" "SOURCES;LIBRARIES" ${ARGN})

    add_executable(${NAME} ${ARG_SOURCES})
    set_target_properties(${NAME} PROPERTIES SUFFIX ".elf")
    target_link_libraries(${NAME} PRIVATE Startup ${ARG_LIBRARIES})

    target_link_options(${NAME} PRIVATE
        -T${ARG_LINKER_SCRIPT}
        -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${NAME}.map,--cref
    )
    set_target_properties(${NAME} PROPERTIES LINK_DEPENDS ${ARG_LINKER_SCRIPT})

    set(_elf $<TARGET_FILE:${NAME}>)
    set(_bin ${CMAKE_CURRENT_BINARY_DIR}/${NAME}.bin)
    set(_hex ${CMAKE_CURRENT_BINARY_DIR}/${NAME}.hex)

    add_custom_command(TARGET ${NAME} POST_BUILD
        COMMAND ${ARM_OBJCOPY} -O binary ${_elf} ${_bin}
        COMMAND ${ARM_OBJCOPY} -O ihex   ${_elf} ${_hex}
        COMMAND ${ARM_SIZE} ${_elf}
        BYPRODUCTS ${_bin} ${_hex}
        VERBATIM
        COMMENT "Objcopy ${NAME} -> bin/hex")

    # flash-<name>: generate a J-Link commander script, then run it.
    set(_jlink_script ${CMAKE_CURRENT_BINARY_DIR}/${NAME}.jlink)
    add_custom_target(flash-${NAME}
        COMMAND ${CMAKE_COMMAND}
            -DJLINK_HEX=${_hex}
            -DJLINK_OUT=${_jlink_script}
            -P ${_STM32_CMAKE_DIR}/gen_jlink_script.cmake
        COMMAND ${JLINK_EXE} -device STM32G031F8 -if SWD -speed 4000 -autoconnect 1 -NoGui 1 -CommanderScript ${_jlink_script}
        DEPENDS ${NAME}
        USES_TERMINAL
        VERBATIM
        COMMENT "Flashing ${NAME} with J-Link")

    if(ARG_BOOTLOADER)
        set(_boot_hex $<TARGET_FILE_DIR:${ARG_BOOTLOADER}>/${ARG_BOOTLOADER}.hex)
        set(_full_hex ${CMAKE_CURRENT_BINARY_DIR}/${NAME}-full.hex)

        add_custom_command(OUTPUT ${_full_hex}
            COMMAND ${CMAKE_COMMAND}
                -DHEX_OUT=${_full_hex}
                "-DHEX_IN=${_boot_hex};${_hex}"
                -P ${_STM32_CMAKE_DIR}/merge_hex.cmake
            DEPENDS ${NAME} ${ARG_BOOTLOADER}
            VERBATIM
            COMMENT "Merging ${ARG_BOOTLOADER} + ${NAME} -> ${NAME}-full.hex")
        add_custom_target(${NAME}-full ALL DEPENDS ${_full_hex})

        set(_jlink_full ${CMAKE_CURRENT_BINARY_DIR}/${NAME}-full.jlink)
        add_custom_target(flash-${NAME}-full
            COMMAND ${CMAKE_COMMAND}
                -DJLINK_HEX=${_full_hex}
                -DJLINK_OUT=${_jlink_full}
                -P ${_STM32_CMAKE_DIR}/gen_jlink_script.cmake
            COMMAND ${JLINK_EXE} -device STM32G031F8 -if SWD -speed 4000 -autoconnect 1 -NoGui 1 -CommanderScript ${_jlink_full}
            DEPENDS ${NAME}-full
            USES_TERMINAL
            VERBATIM
            COMMENT "Flashing ${NAME}-full (bootloader + app) with J-Link")
    endif()
endfunction()
