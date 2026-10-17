# A2 — Real CAN drivers, STM32 drive node, ESP32-S3 node template

**Who:** person A. **Needs:** A1 merged. **Folders:** `firmware/drivers/{bxcan,fdcan,twai}/`,
`firmware/nodes/drive_stm32/`, `firmware/nodes/sensor_esp32s3/`, the firmware CI job.

## Goal

Make the core run on real chips: STM32 bxCAN and FDCAN (classic mode) drivers, an ESP32-S3 TWAI
driver, a drive node application, and an ESP32-S3 node template. We don't know the exact STM32
part yet, so support both peripheral families.

## Read first

`docs/architecture.md` sections: "Controller setup per device" (STM32, ESP32-S3, hardware
filters), "Physical layer" (the TCAN1044AV pin table), and "Node firmware" (control cycle,
CAN handling).

## Build

1. **bxCAN driver** (STM32 HAL underneath): 500 kbps timing chosen from a small table keyed on the
   CAN clock (the rows in the doc), with prescaler/BS1/BS2 checked at compile time. Two mask
   filters (node-addressed and class 0). ABOM on, TXFP on, NART off. Interrupt-driven RX into the
   core's ring buffer. Error counters and bus-off reporting.
2. **FDCAN driver**, classic frames only, same features; on bus-off, clear INIT to recover.
3. **TWAI driver** (ESP-IDF): 500 kbps, dual-filter mode with the same two filters (remember
   1 = don't care), RX task pinned to core 1, bus-off alert → recovery → restart.
4. **Drive node application** (`firmware/nodes/drive_stm32/`): two motor channels, encoder
   counting, a 1 kHz PID loop on a timer using the latest setpoint, outputs disabled unless ARMED,
   DRIVE_FB_A / _B after each SYNC, telemetry pages. Pin and timer assignments in one
   `board_config.h` so they're easy to change. Build for one F4 part (e.g. STM32F446) and one G4
   part (e.g. STM32G431) to prove both drivers compile.
5. **TCAN1044AV handling**: STB pin driven low at start-up (a GPIO in `board_config.h`).
6. **ESP32-S3 template** (`firmware/nodes/sensor_esp32s3/`): ESP-IDF project using the core and
   TWAI driver, sending heartbeats and an example sensor frame. Partition table for 4 MB flash as
   described in "Firmware updates over CAN" (two OTA slots, coredump).

## Done when

- Both STM32 builds compile with `arm-none-eabi-gcc` (CMake + the ST HAL/LL headers fetched in
  CI), no warnings.
- The ESP32-S3 project builds in CI using the official `espressif/idf` Docker image (it is fine
  if ESP-IDF can't be installed in this VM).
- Host tests from A1 still pass.
- `firmware/nodes/drive_stm32/README.md` says how to flash it with an ST-Link and which pins to
  set in `board_config.h`.

## Out of scope

Bootloader, OTA, diagnostics, debug levels.
