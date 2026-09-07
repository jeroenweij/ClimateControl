set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)

set(CPU_FLAGS "-mcpu=cortex-m0plus -mthumb -mfloat-abi=soft")
set(CMAKE_C_FLAGS "${CPU_FLAGS} -ffunction-sections -fdata-sections" CACHE STRING "")
set(CMAKE_CXX_FLAGS "${CPU_FLAGS} -ffunction-sections -fdata-sections" CACHE STRING "")
set(CMAKE_ASM_FLAGS "${CPU_FLAGS}" CACHE STRING "")

# No linker script/startup file exists yet (Software/Modules/* firmware images
# are out of scope until this is written) -- so CMake's own compiler sanity
# check, which by default tries to link a full executable, would fail here
# even though the compiler/flags themselves are fine. This tells it to accept
# a static library instead.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
