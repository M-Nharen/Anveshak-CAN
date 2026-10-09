# anv-flash

Reflash rover nodes over CAN from either host or a laptop, through each STM32 node's CAN
bootloader (`firmware/bootloader_stm32/`). docs/architecture.md, "Host flashing tool".

```sh
pip install -e host/tools/anv_flash          # python-can, can-isotp, cantools

anv-flash list                                # all nodes: ID, type, state, firmware build
anv-flash --host orin flash --node 4 drive_app_g431.bin
anv-flash --host orin flash --type drive drive_app_g431.bin   # every drive node, one at a time
anv-flash --host tool rescue --uid 0x1234ABCD drive_app_g431.bin
```

Options: `--interface`/`--channel`/`--bitrate` (python-can; default `socketcan` on `can0`, or
`$ANV_CAN_INTERFACE`/`$ANV_CAN_CHANNEL`), `--host orin|rpi|tool` (or `$ANV_HOST`), `--dbc`.
Exit codes: 0 done, 1 failed, 2 refused for safety.

Images are the stamped `.bin` files the firmware build produces
(`build-*/nodes/drive_stm32/drive_app_<board>.bin`). `python anv_flash/image.py show FILE`
prints a file's header.

## What it does

1. **Refuses** unless this host's HOST_HB says MASTER, and every node heard is DISARMED (or
   already in its bootloader). Also refuses an image whose node type doesn't match the node.
2. Sends NMT ENTER_BOOTLOADER (as the master, with its term) and waits for the node's
   heartbeat with state BOOTLOADER. A node with no heartbeat at all (crashing right after
   boot) is caught in its 300 ms boot window: power-cycle it when asked.
3. INFO, then ERASE (one erase unit per request, so each finishes within the 5 s timeout),
   WRITE in 4 KB blocks, VERIFY (CRC32), COMMIT (header written last), BOOT. Any failure after
   the first WRITE sends ABORT, so the node is left in its bootloader with nothing half-valid.
4. Waits for the node's application heartbeat and checks its build ID matches the image.

`rescue --uid` finds the board on the node-31 rescue channel with SELECT, whatever its node ID
(stuck in the bootloader, crash-looping, or unconfigured on ID 31), then flashes it the same
way. `anv-flash list` shows the uid of every node currently in its bootloader.

As a library (e.g. for the dashboard): `anv_flash.Flasher(bus, db, host_id, progress=print)`
with `flash_node()`, `flash_type()`, `rescue()`, `list_nodes()`.

## Tests

```sh
pip install -r host/tools/anv_flash/requirements.txt
pytest host/tools/anv_flash
```

`tests/fake_node.py` simulates nodes (application + bootloader) on a python-can virtual bus.
`tests/test_real_bootloader.py` flashes the real C bootloader, built for Linux; it runs when
`ANV_BL_STDIO_NODE` points at `bl_stdio_node` from the firmware build (CI does this).

## Not done yet

- "Quieter bus" (asking nodes to pause telemetry while flashing) is not sent; the transfer
  works without it, only slower on a busy bus.
- ESP32-S3 nodes update through their application (A/B partitions), not this bootloader.
