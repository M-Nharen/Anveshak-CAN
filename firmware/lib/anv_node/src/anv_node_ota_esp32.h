/* ESP32 / ESP32-S3 OTA backend for anv_node_ota (Arduino-ESP32 3.3+ or ESP-IDF 5.5): writes the
 * image to the partition this app isn't running from, using esp_ota_ops.
 *
 *   cfg.ota = anv_node_ota_esp32_backend();
 *   cfg.ota_ctx = NULL; // the backend keeps its own state; ota_ctx is unused
 *
 * Needs two `ota` partitions in the partition table (the Arduino-ESP32 "Default" scheme and
 * ESP-IDF's factory+2×ota layouts both have them). Off the ESP32 this compiles to nothing, so
 * every build can list the source file.
 */
#ifndef ANV_NODE_OTA_ESP32_H
#define ANV_NODE_OTA_ESP32_H

#include "anv_node_ota.h"

#ifdef __cplusplus
extern "C" {
#endif

const anv_node_ota_backend_t *anv_node_ota_esp32_backend(void);

#ifdef __cplusplus
}
#endif

#endif /* ANV_NODE_OTA_ESP32_H */
