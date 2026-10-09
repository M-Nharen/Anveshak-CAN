# A3 — STM32 CAN bootloader and `anv-flash` tool

**Who:** person A. **Needs:** A2 merged. **Folders:** `firmware/bootloader_stm32/`,
`host/tools/anv_flash/`, linker scripts in `firmware/nodes/drive_stm32/`. Opus may be worth
using here if Sonnet gets stuck.

## Goal

Reflash any STM32 node over CAN from either host, safely, with no way to brick a node.

## Read first

`docs/architecture.md` section "Firmware updates over CAN", all of it except the ESP32-S3 parts.

## Build

1. **Bootloader** (≤ 32 KB), using the core's CAN drivers at 500 kbps: the boot sequence exactly
   as in the doc (stay-in-bootloader flag, 300 ms listen window, header check with node type and
   CRC32, jump with VTOR). Motor outputs never enabled.
2. **ISO-TP** on class 7 (sub 0 host → node, sub 1 node → host) using a small library such as
   `isotp-c`.
3. Commands INFO, ERASE, WRITE (≤ 4 KB blocks, read back), VERIFY, COMMIT (header written last),
   BOOT, ABORT, with the error codes.
4. Flash layouts for both cases in the doc (2 KB-page parts and F4 sector parts), and the
   application linker scripts and header for the drive node.
5. Write-protect instructions (option bytes) documented in the README.
6. **`anv-flash`** (Python, `python-can` + `can-isotp`): `list`, `flash --node N file.bin`,
   `flash --type drive file.bin` (one at a time), `rescue --uid`. Refuses unless every node is
   DISARMED and this host leads. Waits for the node's heartbeat and checks the build ID after BOOT.
7. Wire ENTER_BOOTLOADER in the core to set the flag and reset.

## Done when

- Bootloader and application compile for the F446 and G431 targets.
- A host-side test of the bootloader's command logic (built for Linux with a fake flash and the
  loopback driver) covers: normal flash, interrupted flash (stays in bootloader), wrong node
  type (rejected), CRC mismatch (rejected).
- `anv-flash` has unit tests against a fake node.
