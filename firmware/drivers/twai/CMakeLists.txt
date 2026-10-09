# ESP-IDF component registration. Unlike drivers/bxcan and drivers/fdcan (plain CMake
# libraries, built cross-compiled with fetched ST headers), TWAI only ever exists on the
# ESP32-S3 and only ever builds inside an ESP-IDF project (firmware/nodes/sensor_esp32s3), so
# this file is the idf_component_register() form directly -- there is no separate plain-CMake
# build to support.
idf_component_register(
  SRCS "src/acc_can_twai.c"
  INCLUDE_DIRS "include"
  REQUIRES driver anveshak_can_core
)
