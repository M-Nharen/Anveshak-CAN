# Only buildable cross-compiled for an STM32 with an FDCAN peripheral (G0/G4/H7), with the
# matching ST CMSIS device headers and HAL driver headers made available by the caller via
# ANV_STM32_CMSIS_CORE_DIR (shared ARM CMSIS core, any family), ANV_STM32_FDCAN_CMSIS_DEVICE_DIR
# and ANV_STM32_FDCAN_HAL_DIR (see firmware/nodes/drive_stm32/README.md). Skipped entirely
# otherwise, so this subdirectory is always safe to add -- including alongside a bxCAN build
# for a different family in the same invocation, since these variable names are FDCAN-specific
# and never collide with drivers/bxcan's.
if(NOT (CMAKE_CROSSCOMPILING AND ANV_STM32_CMSIS_CORE_DIR AND ANV_STM32_FDCAN_CMSIS_DEVICE_DIR AND ANV_STM32_FDCAN_HAL_DIR))
  return()
endif()

add_library(acc_can_fdcan STATIC
  src/acc_can_fdcan.c
)

target_include_directories(acc_can_fdcan PUBLIC
  include
  ${ANV_STM32_CMSIS_CORE_DIR}
  ${ANV_STM32_FDCAN_CMSIS_DEVICE_DIR}
  ${ANV_STM32_FDCAN_HAL_DIR}/Inc
)

# stm32g4xx_hal.h always does `#include "stm32g4xx_hal_conf.h"`; ANV_STM32_FDCAN_CONF_DIR
# points at the board directory holding that board's copy (firmware/nodes/drive_stm32/boards/*).
if(ANV_STM32_FDCAN_CONF_DIR)
  target_include_directories(acc_can_fdcan PUBLIC ${ANV_STM32_FDCAN_CONF_DIR})
endif()

target_link_libraries(acc_can_fdcan PUBLIC anveshak_can_core)

target_compile_definitions(acc_can_fdcan PUBLIC USE_HAL_DRIVER)

if(ANV_STM32_FDCAN_DEVICE_DEFINE)
  target_compile_definitions(acc_can_fdcan PUBLIC ${ANV_STM32_FDCAN_DEVICE_DEFINE})
endif()
if(ANV_CAN_FDCAN_FAMILY)
  target_compile_definitions(acc_can_fdcan PUBLIC ACC_CAN_FDCAN_FAMILY_${ANV_CAN_FDCAN_FAMILY}=1)
endif()
if(ANV_CAN_FDCAN_CLOCK_HZ)
  target_compile_definitions(acc_can_fdcan PUBLIC ACC_CAN_FDCAN_CLOCK_HZ=${ANV_CAN_FDCAN_CLOCK_HZ})
endif()
