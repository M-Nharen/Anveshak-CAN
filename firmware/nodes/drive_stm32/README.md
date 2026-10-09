# Drive node firmware (STM32)

Two board variants of the same application, proving the shared `anveshak_can_core` library
runs on both STM32 CAN peripheral families (docs/architecture.md, "Controller setup per
device"):

| Variant | MCU | CAN peripheral | Driver |
| --- | --- | --- | --- |
| F446 | STM32F446 | bxCAN | `firmware/drivers/bxcan` |
| G431 | STM32G431 | FDCAN, classic mode | `firmware/drivers/fdcan` |

Every pin, timer and clock assignment lives in one file per board:
`boards/f446/board_config_f446.h` and `boards/g431/board_config_g431.h`. Edit those (not
`app/` or `src/main_*.c`) to match a real PCB.

## What's here

- `app/` — portable motor PID/ramp/mode logic (`drive_motor.c`) and the `anv_node_t` wiring
  (`drive_app.c`): no HAL dependency, builds and is unit-tested on the host.
- `boards/f446/`, `boards/g431/` — pin/timer assignments and the trimmed HAL configuration
  (`stm32*_hal_conf.h`) for each variant.
- `src/main_f446.c` / `src/board_hw_f446.c` and the `_g431` equivalents — board bring-up
  (GPIO, TIM PWM, TIM encoder, ADC, IWDG) and the main loop, built against the real ST HAL.
- `tests/` — host-side unit tests (`ctest`), run with the loopback CAN driver, same as the
  core library's own tests.

## Building

### Host tests (no toolchain needed beyond a C compiler)

```sh
cmake -S firmware -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

### Cross-compiling a board variant

Both `src/main_f446.c`/`src/main_g431.c` and the `acc_can_bxcan`/`acc_can_fdcan` drivers need
the real ST CMSIS device headers and HAL driver headers on the include path. CI fetches just
those two split repos per family (not the full `STM32Cube*` super-repos, which vendor them as
submodules) plus the family-independent ARM CMSIS core headers:

```sh
git clone --depth 1 https://github.com/STMicroelectronics/cmsis_device_f4.git cmsis_device_f4
git clone --depth 1 https://github.com/STMicroelectronics/stm32f4xx_hal_driver.git stm32f4xx_hal_driver
git clone --depth 1 --filter=blob:none --sparse https://github.com/STMicroelectronics/STM32CubeF4.git cubef4
git -C cubef4 sparse-checkout set Drivers/CMSIS/Include
```

Then, for the F446 (bxCAN) build:

```sh
cmake -S firmware -B build-f446 \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/firmware/cmake/arm-none-eabi.cmake" \
  -DANV_BUILD_DRIVE_STM32_F446=ON \
  -DANV_STM32_CMSIS_CORE_DIR="$PWD/cubef4/Drivers/CMSIS/Include" \
  -DANV_STM32_BXCAN_CMSIS_DEVICE_DIR="$PWD/cmsis_device_f4/Include" \
  -DANV_STM32_BXCAN_HAL_DIR="$PWD/stm32f4xx_hal_driver" \
  -DANV_STM32_BXCAN_DEVICE_DEFINE=STM32F446xx \
  -DANV_STM32_BXCAN_CONF_DIR="$PWD/firmware/nodes/drive_stm32/boards/f446" \
  -DANV_CAN_BXCAN_FAMILY=F4 \
  -DANV_CAN_BXCAN_CLOCK_HZ=45000000
cmake --build build-f446
```

For the G431 (FDCAN) build, swap in `cmsis_device_g4`/`stm32g4xx_hal_driver`, the matching
`ANV_STM32_FDCAN_*` / `ANV_CAN_FDCAN_*` variables (`ANV_STM32_FDCAN_DEVICE_DEFINE=STM32G431xx`,
`ANV_CAN_FDCAN_FAMILY=G4`, `ANV_CAN_FDCAN_CLOCK_HZ=160000000`), `boards/g431` for
`ANV_STM32_FDCAN_CONF_DIR`, and `-DANV_BUILD_DRIVE_STM32_G431=ON`. `.github/workflows/ci.yml`
runs both exactly this way.

The board targets (`drive_stm32_f446`, `drive_stm32_g431`) are static libraries; the same
build then links each into a flashable image that runs behind the CAN bootloader
(`image.cmake`, `ld/drive_app_*.ld`, ST's startup and system files, `src/app_image.c`) and
stamps its header:

| Output (in `build-*/nodes/drive_stm32/`) | What |
| --- | --- |
| `drive_app_<board>.elf` | Linked application, at 0x0801_0000 (F446) / 0x0800_9000 (G431) |
| `drive_app_<board>.bin` | Stamped image for `anv-flash` (header slot + application) |

The bootloader itself (`firmware/bootloader_stm32/`) builds alongside, from the same
`board_config_*.h`, as `build-*/bootloader_stm32/anv_bootloader_<board>.bin`.

## Flashing

Normally over CAN: `anv-flash --host orin flash --node 4 drive_app_g431.bin`
(`host/tools/anv_flash/README.md`). The application handles NMT ENTER_BOOTLOADER (only while
DISARMED) by setting the bootloader's stay flag and resetting.

First time only, over SWD (ST-Link or a Nucleo's on-board one, on SWCLK/SWDIO/GND):

1. Flash the bootloader at 0x0800_0000 and write-protect it
   (`firmware/bootloader_stm32/README.md`, "First-time programming").
2. Either flash `drive_app_<board>.bin` at 0x0801_0000 (F446) / 0x0800_9000 (G431) with
   `st-flash write`, or let the bootloader wait and use `anv-flash`.
3. Set the node ID switch (five pins, `DRIVE_NODE_ID_PIN0`-`DRIVE_NODE_ID_PIN4` in the
   board's `board_config_*.h`) before powering on; the ID is read once at boot.

## Pins to set in `board_config_*.h`

Each board's header groups every assignment under a short comment naming the doc section it
implements. At minimum, before building for a real PCB, set:

- `DRIVE_CAN_GPIO_PORT`/`DRIVE_CAN_TX_PIN`/`DRIVE_CAN_RX_PIN`/`DRIVE_CAN_GPIO_AF` — the
  CAN/FDCAN TX and RX pins and their alternate function.
- `DRIVE_TCAN_STB_PORT`/`DRIVE_TCAN_STB_PIN` — the TCAN1044AV's STB pin (driven low at
  start-up; docs/architecture.md, "Physical layer").
- `DRIVE_NODE_ID_PORT`/`DRIVE_NODE_ID_PIN0`-`PIN4` — the 5-bit node ID switch.
- `DRIVE_MOTOR_A_*` / `DRIVE_MOTOR_B_*` — PWM timer/channel/pin, direction pin, and the
  quadrature encoder timer/pins for each motor channel.
- `DRIVE_PID_TIM`/`DRIVE_PID_TIM_IRQ` — the 1 kHz PID loop timer.
- `DRIVE_CURRENT_ADC`/`DRIVE_CURRENT_A_ADC_CHANNEL`/`DRIVE_CURRENT_B_ADC_CHANNEL` — current
  sense ADC channels.
- `DRIVE_STATUS_LED_PORT`/`DRIVE_STATUS_LED_PIN`.

The matching `stm32*_hal_conf.h` next to each `board_config_*.h` only needs changing if a real
board enables a HAL module this template doesn't (e.g. a second UART for a debug console).

## Known stubs / left for later prompts

- No flash-backed parameter storage yet: `anv_board_hooks_t.storage_load/storage_save` are
  `NULL`, so SERVICE SAVE reports `FLASH_ERROR`. Parameters still work live (GET/SET), they
  just don't survive a reset.
- `DRIVE_TELEM_1`'s `watchdog_reset_count`, `last_reset_cause` and `commands_missed` are not
  wired up (need a backup-register counter/cause and a DRIVE_CMD sequence-gap counter).
- Current-sense and temperature scaling are placeholders (the real gain depends on the
  current-sense amplifier, shunt resistor and temperature sensors on the final board).
- `SystemClock_Config()` in each `main_*.c` is a minimal placeholder; the real PLL
  configuration depends on the final board's crystal and is a CubeMX job once the exact part
  is chosen.
- OTA (ESP32-S3) and debug levels are out of scope for this prompt.
