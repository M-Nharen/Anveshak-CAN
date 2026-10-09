# Only buildable cross-compiled for an STM32 with a bxCAN peripheral (F0/F1/F4), with the
# matching ST CMSIS device headers and HAL driver headers made available by the caller via
# ANV_STM32_CMSIS_CORE_DIR (shared ARM CMSIS core, any family), ANV_STM32_BXCAN_CMSIS_DEVICE_DIR
# and ANV_STM32_BXCAN_HAL_DIR (see firmware/nodes/drive_stm32/README.md). Skipped entirely
# otherwise, so this subdirectory is always safe to add -- including alongside an FDCAN build
# for a different family in the same invocation, since these variable names are bxCAN-specific
# and never collide with drivers/fdcan's.
if(NOT (CMAKE_CROSSCOMPILING AND ANV_STM32_CMSIS_CORE_DIR AND ANV_STM32_BXCAN_CMSIS_DEVICE_DIR AND ANV_STM32_BXCAN_HAL_DIR))
  return()
endif()

add_library(acc_can_bxcan STATIC
  src/acc_can_bxcan.c
)

target_include_directories(acc_can_bxcan PUBLIC
  include
  ${ANV_STM32_CMSIS_CORE_DIR}
  ${ANV_STM32_BXCAN_CMSIS_DEVICE_DIR}
  ${ANV_STM32_BXCAN_HAL_DIR}/Inc
)

# stm32f4xx_hal.h always does `#include "stm32f4xx_hal_conf.h"`; ANV_STM32_BXCAN_CONF_DIR
# points at the board directory holding that board's copy (firmware/nodes/drive_stm32/boards/*).
if(ANV_STM32_BXCAN_CONF_DIR)
  target_include_directories(acc_can_bxcan PUBLIC ${ANV_STM32_BXCAN_CONF_DIR})
endif()

target_link_libraries(acc_can_bxcan PUBLIC anveshak_can_core)

target_compile_definitions(acc_can_bxcan PUBLIC USE_HAL_DRIVER)

if(ANV_STM32_BXCAN_DEVICE_DEFINE)
  target_compile_definitions(acc_can_bxcan PUBLIC ${ANV_STM32_BXCAN_DEVICE_DEFINE})
endif()
if(ANV_CAN_BXCAN_FAMILY)
  target_compile_definitions(acc_can_bxcan PUBLIC ACC_CAN_BXCAN_FAMILY_${ANV_CAN_BXCAN_FAMILY}=1)
endif()
if(ANV_CAN_BXCAN_CLOCK_HZ)
  target_compile_definitions(acc_can_bxcan PUBLIC ACC_CAN_BXCAN_CLOCK_HZ=${ANV_CAN_BXCAN_CLOCK_HZ})
endif()
