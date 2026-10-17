# L5 — flashing over CAN + diagnostics, on the new libraries

Branch: `lib/ota-diag` from `next` (after L3 and L4 are merged). PR into `next`.

## Read first
- The bench phase 2 ESP32 CAN flashing (firmware side and `host/tools/anv_flash/`)
- The A4 diagnostics branch (`claude/a4-node-diagnostics`) for error events, black box, debug levels
- `schema/protocol.yaml` (firmware transfer, error events, debug levels)

## Build
- Firmware `anv_ota`: receive an image over CAN into the other OTA slot, verify, reboot,
  roll back automatically if the new image doesn't confirm itself. Only allowed while disarmed.
- Firmware `anv_diag`: error events, a small black box saved across reboots, debug levels
  changeable from the host.
- Host `anv-flash`: flash one node, all nodes of a type, or a list; progress output;
  works from any Linux host (Pi or Jetson). Includes a rescue path for a crash-looping node.
- Host-side diagnostics: receive and log error events, set debug levels.
- Wire both into `anv_node` so every node gets them automatically.

## Done when
- Linux tests cover image transfer, a corrupted image being rejected, and rollback logic.
- `anv-flash` and diagnostics work against a fake node on vcan0.
- Update `README.md` with the bench demo steps from `prompts/v2/README.md`.
