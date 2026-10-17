/* ESP32 glue for anv_node_ota. Compiles to nothing off the ESP32, so every build can list it
 * (the same convention as anv_node_esp32.c). The rollback decision itself (whether the running
 * image needs to prove itself, or already failed to) is anv_node_ota.c's, kept in flash
 * through cfg.storage; this file only moves bytes into the other slot and sets which slot the
 * bootloader starts next. */
#if defined(ESP_PLATFORM)

#include "anv_node_ota_esp32.h"

#include "esp_ota_ops.h"
#include "esp_system.h"

static esp_ota_handle_t s_handle;
static const esp_partition_t *s_update_part;
static bool s_writing;

static bool esp32_begin(void *ctx, uint32_t image_len)
{
    (void)ctx;
    s_update_part = esp_ota_get_next_update_partition(NULL);
    if (s_update_part == NULL || image_len > s_update_part->size) {
        return false;
    }
    s_writing = esp_ota_begin(s_update_part, image_len, &s_handle) == ESP_OK;
    return s_writing;
}

static bool esp32_write(void *ctx, const uint8_t *data, uint16_t len)
{
    (void)ctx;
    return s_writing && esp_ota_write(s_handle, data, len) == ESP_OK;
}

static bool esp32_commit(void *ctx)
{
    (void)ctx;
    if (!s_writing || esp_ota_end(s_handle) != ESP_OK) {
        s_writing = false;
        return false;
    }
    s_writing = false;
    return esp_ota_set_boot_partition(s_update_part) == ESP_OK;
}

static void esp32_abort(void *ctx)
{
    (void)ctx;
    if (s_writing) {
        (void)esp_ota_abort(s_handle);
        s_writing = false;
    }
}

static void esp32_confirm(void *ctx)
{
    (void)ctx;
    /* Best-effort: only meaningful if the build enabled ESP-IDF's own rollback feature too.
     * anv_node's rollback (below) doesn't depend on it either way. */
    (void)esp_ota_mark_app_valid_cancel_rollback();
}

static void esp32_rollback_and_reboot(void *ctx)
{
    (void)ctx;
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    if (other != NULL && other != running) {
        (void)esp_ota_set_boot_partition(other);
    }
    esp_restart(); /* does not return */
}

static const anv_node_ota_backend_t s_backend = {
    esp32_begin, esp32_write, esp32_commit, esp32_abort, esp32_confirm, esp32_rollback_and_reboot,
};

const anv_node_ota_backend_t *anv_node_ota_esp32_backend(void)
{
    return &s_backend;
}

#else
/* ISO C forbids an empty translation unit. */
typedef int anv_node_ota_esp32_unused_t;
#endif /* ESP_PLATFORM */
