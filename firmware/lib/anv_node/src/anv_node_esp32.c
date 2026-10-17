/* ESP32 glue for anv_node. Compiles to nothing off the ESP32, so every build can list it. */
#if defined(ESP_PLATFORM)

#include "anv_node_esp32.h"

#include <string.h>

#include "esp_mac.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define BOOT_COUNT_KEY "anv_boots"

bool anv_node_nvs_init(anv_node_nvs_t *nvs, const char *ns)
{
    memset(nvs, 0, sizeof *nvs);
    strncpy(nvs->ns, ns, sizeof nvs->ns - 1u);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* The partition is full or from a newer layout: start it again rather than run without. */
        (void)nvs_flash_erase();
        err = nvs_flash_init();
    }
    nvs->ok = err == ESP_OK;
    return nvs->ok;
}

static bool open_ns(void *ctx, nvs_handle_t *h)
{
    const anv_node_nvs_t *nvs = (const anv_node_nvs_t *)ctx;
    return nvs->ok && nvs_open(nvs->ns, NVS_READWRITE, h) == ESP_OK;
}

static size_t store_load(void *ctx, const char *key, void *buf, size_t cap)
{
    nvs_handle_t h;
    if (!open_ns(ctx, &h)) {
        return 0;
    }
    size_t len = cap;
    const esp_err_t err = nvs_get_blob(h, key, buf, &len); /* fails if the value doesn't fit */
    nvs_close(h);
    return err == ESP_OK ? len : 0;
}

static bool store_save(void *ctx, const char *key, const void *buf, size_t len)
{
    nvs_handle_t h;
    if (!open_ns(ctx, &h)) {
        return false;
    }
    const bool ok = nvs_set_blob(h, key, buf, len) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

static bool store_erase(void *ctx, const char *key)
{
    nvs_handle_t h;
    if (!open_ns(ctx, &h)) {
        return false;
    }
    const esp_err_t err = nvs_erase_key(h, key);
    const bool ok = (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

static const anv_node_storage_t nvs_ops = {store_load, store_save, store_erase};

const anv_node_storage_t *anv_node_nvs_ops(void)
{
    return &nvs_ops;
}

void anv_node_esp32_identity(anv_node_config_t *cfg, anv_node_nvs_t *nvs)
{
    (void)esp_efuse_mac_get_default(cfg->mac);
#if defined(CONFIG_IDF_TARGET_ESP32)
    cfg->info.chip = 1u;
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    cfg->info.chip = 2u;
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
    cfg->info.chip = 3u;
#else
    cfg->info.chip = 0u;
#endif
    cfg->info.reset_reason = (uint32_t)esp_reset_reason();
    if (nvs != NULL) {
        uint32_t boots = 0;
        if (store_load(nvs, BOOT_COUNT_KEY, &boots, sizeof boots) != sizeof boots) {
            boots = 0;
        }
        boots++;
        (void)store_save(nvs, BOOT_COUNT_KEY, &boots, sizeof boots);
        cfg->info.boot_count = boots;
    }
}

void anv_node_esp32_restart(anv_node_t *node, void *user)
{
    (void)node;
    (void)user;
    esp_restart();
}

#else
/* ISO C forbids an empty translation unit. */
typedef int anv_node_esp32_unused_t;
#endif
