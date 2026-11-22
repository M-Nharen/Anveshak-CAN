/* Pins for the bench node. One file to edit when wiring a board differently; picked per chip at
 * compile time, so the same sketch builds for a WROOM (classic ESP32) and an ESP32-S3. */
#ifndef BENCH_BOARD_PINS_H
#define BENCH_BOARD_PINS_H

#include <Arduino.h>  // the board variant's pins (RGB_BUILTIN)

#include "sdkconfig.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3)
/* ESP32-S3 boards: the addressable RGB LED moves between boards (GPIO 48 on most DevKitC-1, 38 on
 * v1.1, elsewhere on other makers' boards). The Arduino core's board variant knows it as
 * RGB_BUILTIN, so pick the right board in Tools > Board and the LED follows; 48 otherwise. */
#define BENCH_CAN_TX_GPIO 16
#define BENCH_CAN_RX_GPIO 15
#if defined(RGB_BUILTIN)
#define BENCH_LED_GPIO RGB_BUILTIN
#else
#define BENCH_LED_GPIO 48
#endif
#define BENCH_LED_IS_RGB 1
/* DIP switches (bit 0 first), each between the pin and GND. Clear of CAN, the LED, strapping
 * pins (0, 3, 45, 46), USB (19, 20), flash (26-32) and octal PSRAM (33-37). */
#define BENCH_DIP_PINS {4, 5, 6, 7}
#elif defined(CONFIG_IDF_TARGET_ESP32)
/* WROOM DevKit: plain LED on GPIO 2. GPIO 16/17 are the PSRAM pins on WROVER modules and GPIO 15
 * is a boot strapping pin, so the CAN pins stay clear of them. TX 4 / RX 5 is how the bench WROOMs are
 * wired. */
#define BENCH_CAN_TX_GPIO 4
#define BENCH_CAN_RX_GPIO 5
#define BENCH_LED_GPIO 2
#define BENCH_LED_IS_RGB 0
/* DIP switches (bit 0 first), each between the pin and GND. Clear of CAN, the LED, strapping
 * pins (0, 2, 12, 15), flash (6-11) and the input-only pins without pull-ups (34-39). */
#define BENCH_DIP_PINS {13, 14, 25, 26}
#else
#error "bench_esp32_arduino supports the ESP32 (WROOM) and the ESP32-S3"
#endif

/* Node ID = BENCH_DIP_ID_BASE + the DIP value: 16..31 with four switches. Every board on the bus
 * needs a different setting; two boards on the same one both flag id_conflict and go quiet.
 * Keep IDs clear of the hosts' (1 = Orin, 2 = RPi, 250 = the CLI tools). */
#define BENCH_DIP_ID_BASE 16

/* TCAN STB is tied to GND on the bench boards. If a board drives it from a GPIO instead, set the
 * pin here: the sketch drives it low (normal mode) at boot. -1 = not connected. */
#define BENCH_TCAN_STB_GPIO (-1)

#endif /* BENCH_BOARD_PINS_H */
