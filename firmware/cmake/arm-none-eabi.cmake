# Cross-compile toolchain file for a Cortex-M4 target (e.g. STM32F4), compile-only: there is
# no board support package or linker script here, so this only proves the core library
# builds warning-free for the target architecture (CLAUDE.md / prompts/A1-firmware-core.md
# "Done when"). A real node project (A2) supplies its own toolchain file with the matching
# linker script and HAL.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)

# Checking the compiler by linking a full executable fails with no startup code / linker
# script; a static library is enough to prove the compiler works.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mcpu=cortex-m4 -mthumb -mfloat-abi=soft")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
