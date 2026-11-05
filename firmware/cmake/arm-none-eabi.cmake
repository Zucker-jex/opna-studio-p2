# Bare-metal ARM toolchain for OPNA Studio P2 (STM32F103VE, Cortex-M3).
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(TC_PREFIX arm-none-eabi-)
set(CMAKE_C_COMPILER   ${TC_PREFIX}gcc)
set(CMAKE_ASM_COMPILER ${TC_PREFIX}gcc)
set(CMAKE_OBJCOPY      ${TC_PREFIX}objcopy CACHE INTERNAL "")
set(CMAKE_SIZE         ${TC_PREFIX}size    CACHE INTERNAL "")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_EXECUTABLE_SUFFIX_C   ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_ASM ".elf")

set(MCU_FLAGS "-mcpu=cortex-m3 -mthumb")
set(CMAKE_C_FLAGS_INIT   "${MCU_FLAGS} -ffreestanding -ffunction-sections -fdata-sections")
set(CMAKE_ASM_FLAGS_INIT "${MCU_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "${MCU_FLAGS} -nostartfiles -specs=nosys.specs -Wl,--gc-sections")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
