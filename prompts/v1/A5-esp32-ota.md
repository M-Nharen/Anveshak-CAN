# A5 — ESP32-S3 firmware update over CAN

**Who:** person A. **Needs:** A4 merged. **Folders:** `firmware/nodes/sensor_esp32s3/`,
`host/tools/anv_flash/`.

## Goal

Update ESP32-S3 nodes over CAN with automatic rollback, using the same `anv-flash` tool.

## Read first

`docs/architecture.md` section "Firmware updates over CAN", the ESP32-S3 parts.

## Build

1. In the ESP32-S3 app: receive the image over ISO-TP on class 7 and write it to the inactive OTA
   slot with ESP-IDF's OTA API, verify, set the boot slot, restart.
2. Rollback enabled; mark the app valid only after CAN is up, a master has been seen, and a few
   seconds have passed.
3. Report OTA_ROLLBACK as a NODE_EVENT if a rollback happened.
4. `anv-flash` detects ESP32 nodes (node type from the heartbeat) and uses this path.
5. Optional: HTTP OTA over Wi-Fi in the pit, off by default.

## Done when

- The ESP-IDF project builds in CI.
- `anv-flash` tests cover the ESP32 path against a fake node.
