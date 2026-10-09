# STM32 CAN bootloader

A small permanent bootloader (≤ 32 KB) that lets either host reflash any STM32 node over CAN,
with no way to brick it. Design: `docs/architecture.md`, "Firmware updates over CAN". The host
side is `anv-flash` (`host/tools/anv_flash/`).

## Layout

| Path | What |
| --- | --- |
| `include/anv_bl_proto.h` | Wire protocol and image header: the single reference for both sides |
| `src/anv_bl.c` | Boot sequence, ISO-TP channels, heartbeats (hardware-free) |
| `src/anv_bl_cmd.c` | INFO / ERASE / WRITE / VERIFY / COMMIT / BOOT / ABORT / SELECT (hardware-free) |
| `src/anv_bl_image.c`, `src/anv_bl_flash.c` | Header and CRC32 checks; erase-unit helpers |
| `third_party/isotp-c/` | ISO-TP (ISO 15765-2), vendored, MIT |
| `port/stm32/` | Shared STM32 main loop, jump to the application, UID, stay flag, linker sections |
| `port/f446/`, `port/g431/` | Clocks, CAN, flash backend and linker script per family |
| `tests/` | Host tests (Linux, loopback CAN driver, fake flash), and `bl_stdio_node` for anv-flash's interop test |

The application side lives with each node: for the drive node, `nodes/drive_stm32/ld/` (linker
scripts at the application address), `nodes/drive_stm32/src/app_image.c` (header, hooks) and
`nodes/drive_stm32/image.cmake` (link + stamp).

## Flash layout

| Region | G431 (2 KB pages) | F446 (sectors) | Contents |
| --- | --- | --- | --- |
| Bootloader | 0x0800_0000, pages 0-15 (32 KB) | 0x0800_0000, sectors 0-1 (32 KB) | Write-protected |
| Parameters | 0x0800_8000, pages 16-17 (4 KB) | 0x0800_8000, sectors 2-3 (32 KB) | Saved node parameters |
| Header slot | 0x0800_9000, 512 B | 0x0801_0000, 512 B | Header (64 B used) |
| Application | 0x0800_9200 onward | 0x0801_0200 onward | Vector table first |

**One deviation from the doc:** the header slot is 512 B, not 256 B. VTOR must be aligned to
the vector table's size rounded up to a power of two; the F446 (113 vectors) and G431 (118)
both need 512-byte alignment, so an application at "+0x100" could not point VTOR at its own
table. The header itself is 64 B (see `anv_bl_proto.h`): magic, version, node type, image
length, image CRC32, firmware version, build ID, git hash, header CRC32.

The bootloader's flash backend only covers the header slot and the application. No command can
reach the bootloader or parameter regions, whatever offset a host sends.

## Boot sequence

1. Motor-driver enables are never touched: hardware pull-downs keep them low.
2. Read the node ID switch (0 or unreadable → 31), start CAN at 500 kbps with two filters:
   everything addressed to this node, and the node-31 rescue channel (`0x7F8`).
3. Stay flag set (backup register 0 = `"BLST"`, written by the application on
   ENTER_BOOTLOADER) → clear it, stay.
4. Otherwise listen 300 ms. Any request this bootloader answers keeps it in the bootloader.
5. Check the application: magic, header CRC, node type, length, CRC32 of the whole image, and
   that the vector table's stack pointer and reset handler are sane.
6. Valid → reset clocks and peripherals, set VTOR, jump. Invalid → stay, send one NODE_EVENT
   (`APP_CRC_FAIL` or `WRONG_NODE_TYPE`), then NODE_HB at 10 Hz with state BOOTLOADER and
   `BOOTLOADER_RUNNING` = 1.

## Protocol (ISO-TP on class 7: sub 0 host → node, sub 1 node → host)

Requests start with a command byte; every response starts with
`[command][status][errors.yaml code, u16 LE]` (code 0 on success). Little-endian throughout.
Offsets are relative to the header slot.

| Command | Request | Reply payload |
| --- | --- | --- |
| INFO 0x01 | — | bootloader version, node ID/type, 96-bit UID, uid32, flash size, region base/size, header slot, write alignment, max WRITE, boot status, erase-unit layout, current header |
| ERASE 0x02 | offset u32, length u32 | — (every unit overlapping the range) |
| WRITE 0x03 | offset u32, 1..4096 data bytes | — (programmed, then read back) |
| VERIFY 0x04 | expected CRC32 of the image body | actual CRC32 |
| COMMIT 0x05 | — | — (writes the header, last) |
| BOOT 0x06 | — | — (then resets into the application) |
| ABORT 0x07 | — | — (erases what this session touched, stays) |
| SELECT 0x08 | uid32 u32 | node ID u8, uid32 (only the matching board replies) |

Status codes: OK, MISMATCH, BAD_LENGTH, BAD_RANGE, NOT_ERASED, FLASH_ERROR, WRONG_NODE_TYPE,
BAD_IMAGE, BAD_SEQUENCE, UNKNOWN_COMMAND, mapped to `APP_CRC_FAIL`, `WRONG_NODE_TYPE`,
`FLASH_WRITE_FAIL` or `BAD_FRAME` from `defs/errors.yaml`.

Why a half-written image can never boot: the image's header arrives with the first WRITE
(offset 0) but is only checked (node type!) and kept in RAM. The header slot stays erased until
COMMIT, which is refused unless VERIFY matched both the host's CRC and the header's. A WRITE of
an image for another node type is rejected before anything is programmed.

`uid32` is the CRC32 of the MCU's 96-bit UID; `anv-flash list` shows it for nodes in their
bootloader. On node 31 (several unconfigured boards may share it), and on the rescue channel,
nothing but SELECT is answered until a SELECT names this board.

## Building

Host tests (part of the normal firmware build):

```sh
cmake -S firmware -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

STM32 images: the same cross-compile commands as the drive node
(`firmware/nodes/drive_stm32/README.md`); with `ANV_BUILD_DRIVE_STM32_F446=ON` (or `_G431`) the
build also links:

- `bootloader_stm32/anv_bootloader_f446.elf/.bin` (the link fails above 32 KB; currently ~15 KB),
- `nodes/drive_stm32/drive_app_f446.elf`, and the stamped `drive_app_f446.bin` for anv-flash.

The ST HAL/CMSIS checkouts must be full `cmsis_device_*` and `stm32*xx_hal_driver` clones
(their `Source/Templates` and `Src` trees are used). `ANV_BL_NODE_TYPE` (default 1, drive)
selects which node type a bootloader accepts; pins come from the node's `board_config.h`.

## First-time programming and write protection

Once per board, over SWD:

1. Flash the bootloader: `st-flash write anv_bootloader_g431.bin 0x08000000` (or
   STM32CubeProgrammer). Optionally flash the application too
   (`st-flash write drive_app_g431.bin 0x08009000`; F446: `0x08010000`), or send it later with
   `anv-flash`.
2. Write-protect the bootloader through the option bytes, so a bug or a bad update can never
   erase it:
   - **STM32G431** (WRP areas, 2 KB granularity): protect pages 0-15,
     `STM32_Programmer_CLI -c port=SWD -ob WRP1A_STRT=0x0 WRP1A_END=0xF`
   - **STM32F446** (nWRP bit per sector, 0 = protected): protect sectors 0 and 1,
     `STM32_Programmer_CLI -c port=SWD -ob nWRP0=0 nWRP1=0`
   - Check with `STM32_Programmer_CLI -c port=SWD -ob displ`.
3. Leave RDP at level 0 during development. Level 1 still allows CAN updates but blocks SWD
   reads; level 2 is permanent — don't use it.

To update the bootloader itself later, remove the protection over SWD first
(`WRP1A_STRT=0x7F WRP1A_END=0x0` on G4, i.e. start > end; `nWRP0=1 nWRP1=1` on F4).

## Not covered here

- The real clock tree depends on the final board's crystal: `port/*/anv_bl_board_*.c` assume
  the HSE values in the board's `stm32*_hal_conf.h` (8 MHz F446, 24 MHz G431).
- An F4 128 KB sector erase stalls the CPU for 1-2 s. If the IWDG's hardware-start option bit
  is set, its timeout must cover that.
- STM32 code is compiled, not run, in CI; the command logic, boot sequence and ISO-TP are
  tested on Linux (`tests/`), and anv-flash is tested against this same code
  (`host/tools/anv_flash/tests/test_real_bootloader.py`).
