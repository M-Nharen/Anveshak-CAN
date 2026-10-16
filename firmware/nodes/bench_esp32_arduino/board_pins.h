/* Pins for the bench node. One file to edit when wiring a board differently; picked per chip at
 * compile time, so the same sketch builds for a WROOM (classic ESP32) and an ESP32-S3. */
#ifndef BENCH_BOARD_PINS_H
#define BENCH_BOARD_PINS_H

#include "sdkconfig.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3)
/* ESP32-S3 DevKitC: addressable RGB LED on GPIO 48 (on some board revisions it is GPIO 38). */
#define BENCH_CAN_TX_GPIO 16
#define BENCH_CAN_RX_GPIO 15
#define BENCH_LED_GPIO 48
#define BENCH_LED_IS_RGB 1
#define BENCH_HW_TARGET 1u /* NODE_ID_REQUEST HW_TARGET: ESP32-S3 */
#elif defined(CONFIG_IDF_TARGET_ESP32)
/* WROOM DevKit: plain LED on GPIO 2. GPIO 16/17 are the PSRAM pins on WROVER modules and GPIO 15
 * is a boot strapping pin, so the CAN pins stay clear of them. */
#define BENCH_CAN_TX_GPIO 5
#define BENCH_CAN_RX_GPIO 4
#define BENCH_LED_GPIO 2
#define BENCH_LED_IS_RGB 0
#define BENCH_HW_TARGET 0u /* NODE_ID_REQUEST HW_TARGET: ESP32 */
#else
#error "bench_esp32_arduino supports the ESP32 (WROOM) and the ESP32-S3"
#endif

/* TCAN STB is tied to GND on the bench boards. If a board drives it from a GPIO instead, set the
 * pin here: the sketch drives it low (normal mode) at boot. -1 = not connected. */
#define BENCH_TCAN_STB_GPIO (-1)

#endif /* BENCH_BOARD_PINS_H */
