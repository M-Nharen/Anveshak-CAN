/* ESP32 / ESP32-S3 glue for anv_node (Arduino-ESP32 3.3+ or ESP-IDF 5.5): settings and the node
 * ID in NVS, and the board's identity (MAC, chip, reset reason, boot count).
 *
 *   static anv_node_nvs_t nvs;
 *   anv_node_nvs_init(&nvs, "anv_node");
 *   cfg.storage = anv_node_nvs_ops();
 *   cfg.storage_ctx = &nvs;
 *   anv_node_esp32_identity(&cfg, &nvs);
 *   cfg.on_reset = anv_node_esp32_restart;
 */
#ifndef ANV_NODE_ESP32_H
#define ANV_NODE_ESP32_H

#include "anv_node.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ns[16]; /* NVS namespace */
    bool ok;
} anv_node_nvs_t;

/* Initialise NVS if nobody has yet (Arduino already has) and remember the namespace. */
bool anv_node_nvs_init(anv_node_nvs_t *nvs, const char *ns);
const anv_node_storage_t *anv_node_nvs_ops(void);

/* Fill cfg->mac and cfg->info (chip, reset reason, boot count; the boot count is kept in `nvs`,
 * which may be NULL). fw_build and fw_version are the application's. */
void anv_node_esp32_identity(anv_node_config_t *cfg, anv_node_nvs_t *nvs);

/* An on_reset hook: restart the chip. */
void anv_node_esp32_restart(anv_node_t *node, void *user);

#ifdef __cplusplus
}
#endif

#endif /* ANV_NODE_ESP32_H */
