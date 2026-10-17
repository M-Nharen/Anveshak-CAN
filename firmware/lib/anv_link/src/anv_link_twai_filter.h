/* How anv_link filters map onto the ESP32 TWAI acceptance filter. Plain C with no ESP-IDF
 * headers, so the mapping is unit-tested on Linux.
 *
 * The ESP32 and ESP32-S3 have one acceptance filter. It can be one 29-bit id/mask pair, or two
 * pairs that compare only ID bits 28..13 ("dual" mode). Whatever is programmed must accept a
 * superset of what the exact filters accept; anv_link then applies them exactly in software. */
#ifndef ANV_LINK_TWAI_FILTER_H
#define ANV_LINK_TWAI_FILTER_H

#include "anv_link.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ANV_TWAI_DUAL_SHIFT 13u /* dual mode compares ID bits 28..13 */

/* Register values in esp_twai's twai_mask_filter_config_t format (mask bit 1 = must match).
 * enabled == false: accept everything (no filter programmed). */
typedef struct {
    bool enabled;
    bool dual;
    uint32_t id;
    uint32_t mask;
} anv_twai_hw_filter_t;

anv_twai_hw_filter_t anv_twai_hw_filter(const anv_link_filter_t *filters, uint8_t n);

/* What the hardware would accept with `hw` programmed (a model, for tests). */
bool anv_twai_hw_accepts(const anv_twai_hw_filter_t *hw, uint32_t id);

#ifdef __cplusplus
}
#endif

#endif /* ANV_LINK_TWAI_FILTER_H */
