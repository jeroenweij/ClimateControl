set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)

set(CPU_FLAGS "-mcpu=cortex-m0plus -mthumb -mfloat-abi=soft")
set(CMAKE_C_FLAGS "${CPU_FLAGS} -ffunction-sections -fdata-sections" CACHE STRING "")
set(CMAKE_CXX_FLAGS "${CPU_FLAGS} -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -fno-unwind-tables -fno-use-cxa-atexit" CACHE STRING "")
set(CMAKE_ASM_FLAGS "${CPU_FLAGS}" CACHE STRING "")

# Executable link: our startup + newlib-nano + syscall stubs, dead-code strip,
# and a memory-usage report. The per-module linker script (-T ...) is added by
# add_stm32_executable() in cmake/stm32.cmake.
set(CMAKE_EXE_LINKER_FLAGS
    "${CPU_FLAGS} --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections -Wl,--print-memory-usage -Wl,--no-warn-rwx-segments"
    CACHE STRING "")

# No linker script/startup file exists yet (Software/Modules/* firmware images
# are out of scope until this is written) -- so CMake's own compiler sanity
# check, which by default tries to link a full executable, would fail here
# even though the compiler/flags themselves are fine. This tells it to accept
# a static library instead.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
