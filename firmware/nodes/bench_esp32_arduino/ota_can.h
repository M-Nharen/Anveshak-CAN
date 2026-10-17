/* Firmware update over CAN for the bench node (docs/architecture.md, "Firmware updates over CAN").
 *
 * The running sketch receives the new image over ISO-TP on class 7 (host -> node sub 0, node -> host
 * sub 1), writes it into the spare OTA slot, checks it, and reboots into it. The new image stays on
 * probation until ota_confirm() sees CAN up, a host heard and 5 s of running; if it crashes first, the
 * ESP32 boots the previous slot by itself (rollback is on in the Arduino core's bootloader). */
#ifndef BENCH_OTA_CAN_H
#define BENCH_OTA_CAN_H

#include <stdint.h>

#include "src/anv/acc_can.h"

struct OtaHooks {
  bool (*send)(const acc_can_frame_t& frame);  // queue a frame to the host
  bool (*armed)();                             // true while the node is ARMED: flashing is refused
};

void ota_init(uint8_t node_id, const OtaHooks& hooks);
// Feed every received frame; true if it was an update frame for this node (consumed).
bool ota_on_frame(const acc_can_frame_t& frame, uint32_t now_ms);
// Call every loop: transfer timeouts and the restart after BOOT.
void ota_tick(uint32_t now_ms);
bool ota_busy();

// Call once in setup(), before ota_init(): did the last update boot, or did it roll back?
void ota_check_last_update();
bool ota_rolled_back();  // the last update rolled back (report it once the node is running)
// Call every loop: marks a freshly flashed image valid once it has proven itself.
void ota_confirm(uint32_t now_ms, bool host_heard);

// First two bytes of the ELF SHA-256 the build embeds in the image (offset 0xB0 of the .bin), so
// the host can tell which image a node runs: anv-flash reads the same bytes from the file.
uint16_t ota_fw_build_id();

#endif /* BENCH_OTA_CAN_H */
