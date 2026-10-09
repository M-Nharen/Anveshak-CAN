# Helpers for linking real STM32 images (bootloader and application): ST's HAL sources, the
# CMSIS startup/system files and the link step. Uses the same ANV_STM32_* variables the drive
# node and CAN drivers already take (firmware/nodes/drive_stm32/README.md); the HAL and CMSIS
# device repos those point at include their Src/ and Source/Templates/ trees.

# Plain variables are directory-scoped, so they are set on every include (before the guard).
get_filename_component(ANV_BL_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
get_filename_component(ANV_REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../.. ABSOLUTE)
set(ANV_IMAGE_TOOL ${ANV_REPO_ROOT}/host/tools/anv_flash/anv_flash/image.py)
set(ANV_STM32_SIZE_FLAGS -Os -ffunction-sections -fdata-sections)
enable_language(ASM) # per directory too: ST's startup files are assembly

include_guard(GLOBAL)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(NOT CMAKE_OBJCOPY)
  find_program(CMAKE_OBJCOPY arm-none-eabi-objcopy REQUIRED)
endif()

# anv_stm32_family(<F446|G431>): sets ANV_FAM (F4/G4), ANV_FAM_HAL_LIB, ANV_FAM_STARTUP,
# ANV_FAM_SYSTEM, ANV_FAM_CAN_SOURCE/INCLUDE/DEFINES in the caller's scope, creating the
# family's HAL library (-Os, section GC) on first use.
function(anv_stm32_family board)
  if(board STREQUAL "F446")
    set(fam F4)
    set(prefix stm32f4xx)
    set(dev ${ANV_STM32_BXCAN_CMSIS_DEVICE_DIR})
    set(hal ${ANV_STM32_BXCAN_HAL_DIR})
    set(conf ${ANV_STM32_BXCAN_CONF_DIR})
    set(define ${ANV_STM32_BXCAN_DEVICE_DEFINE})
    set(can_src ${CMAKE_SOURCE_DIR}/drivers/bxcan/src/acc_can_bxcan.c)
    set(can_inc ${CMAKE_SOURCE_DIR}/drivers/bxcan/include)
    set(can_defs ACC_CAN_BXCAN_FAMILY_${ANV_CAN_BXCAN_FAMILY}=1 ACC_CAN_BXCAN_CLOCK_HZ=${ANV_CAN_BXCAN_CLOCK_HZ})
  elseif(board STREQUAL "G431")
    set(fam G4)
    set(prefix stm32g4xx)
    set(dev ${ANV_STM32_FDCAN_CMSIS_DEVICE_DIR})
    set(hal ${ANV_STM32_FDCAN_HAL_DIR})
    set(conf ${ANV_STM32_FDCAN_CONF_DIR})
    set(define ${ANV_STM32_FDCAN_DEVICE_DEFINE})
    set(can_src ${CMAKE_SOURCE_DIR}/drivers/fdcan/src/acc_can_fdcan.c)
    set(can_inc ${CMAKE_SOURCE_DIR}/drivers/fdcan/include)
    set(can_defs ACC_CAN_FDCAN_FAMILY_${ANV_CAN_FDCAN_FAMILY}=1 ACC_CAN_FDCAN_CLOCK_HZ=${ANV_CAN_FDCAN_CLOCK_HZ})
  else()
    message(FATAL_ERROR "anv_stm32_family: unknown board ${board}")
  endif()

  string(TOLOWER ${define} device_lower)
  set(startup ${dev}/../Source/Templates/gcc/startup_${device_lower}.s)
  set(system ${dev}/../Source/Templates/system_${prefix}.c)
  foreach(f ${startup} ${system})
    if(NOT EXISTS ${f})
      message(FATAL_ERROR "${f} not found: the CMSIS device directory must be a full cmsis_device_* checkout")
    endif()
  endforeach()
  set_source_files_properties(${startup} PROPERTIES LANGUAGE ASM COMPILE_OPTIONS "-mcpu=cortex-m4;-mthumb")

  set(lib anv_stm32_hal_${fam})
  if(NOT TARGET ${lib})
    file(GLOB hal_sources ${hal}/Src/${prefix}_hal*.c)
    list(FILTER hal_sources EXCLUDE REGEX "_template\\.c$")
    add_library(${lib} STATIC ${hal_sources})
    target_include_directories(${lib} PUBLIC ${ANV_STM32_CMSIS_CORE_DIR} ${dev} ${hal}/Inc ${conf})
    target_compile_definitions(${lib} PUBLIC USE_HAL_DRIVER ${define})
    # The boards' trimmed stm32*_hal_conf.h leave out a few values only some HAL sources
    # need; ST's template defaults, and only if the conf file doesn't set them.
    target_compile_definitions(${lib} PRIVATE EXTERNAL_CLOCK_VALUE=12288000U)
    # ST's code: built as shipped, its warnings are not ours to fix.
    target_compile_options(${lib} PRIVATE ${ANV_STM32_SIZE_FLAGS} -w)
  endif()

  set(ANV_FAM ${fam} PARENT_SCOPE)
  set(ANV_FAM_HAL_LIB ${lib} PARENT_SCOPE)
  set(ANV_FAM_STARTUP ${startup} PARENT_SCOPE)
  set(ANV_FAM_SYSTEM ${system} PARENT_SCOPE)
  set(ANV_FAM_CAN_SOURCE ${can_src} PARENT_SCOPE)
  set(ANV_FAM_CAN_INCLUDE ${can_inc} PARENT_SCOPE)
  set(ANV_FAM_CAN_DEFINES ${can_defs} PARENT_SCOPE)
endfunction()

# anv_stm32_link(<target> <linker script>): link flags, .elf suffix, map file, and a .bin
# next to the .elf after every build.
function(anv_stm32_link target linker_script)
  set_target_properties(${target} PROPERTIES SUFFIX ".elf" LINK_DEPENDS ${linker_script})
  target_link_options(${target} PRIVATE
    --specs=nano.specs --specs=nosys.specs
    -Wl,--gc-sections -Wl,--print-memory-usage
    -Wl,-Map=$<TARGET_FILE_DIR:${target}>/${target}.map
    -T${linker_script} -L${ANV_BL_ROOT_DIR}/port/stm32)
endfunction()
