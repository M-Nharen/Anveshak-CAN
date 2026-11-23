/* ESP32 board start-up for anv_node. Compiles to nothing off the ESP32, so every build can list it. */
#if defined(ESP_PLATFORM)

#include "anv_board_esp32.h"

#include <string.h>

#include "anv_node_ota_esp32.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#if defined(ARDUINO) && defined(__has_include)
#if __has_include("pins_arduino.h")
#include "pins_arduino.h" /* the board variant's PIN_RGB_LED */
#endif
#endif

/* Per-chip defaults. Each can be overridden for a build with -D<name>=<value>. */
#if defined(CONFIG_IDF_TARGET_ESP32S3)
/* S3 DevKits: TX 16 / RX 15. Switches clear of CAN, the LED, strapping pins (0, 3, 45, 46),
 * USB (19, 20), flash (26-32) and octal PSRAM (33-37). */
#ifndef ANV_BOARD_CAN_TX
#define ANV_BOARD_CAN_TX 16
#endif
#ifndef ANV_BOARD_CAN_RX
#define ANV_BOARD_CAN_RX 15
#endif
#ifndef ANV_BOARD_LED
#if defined(PIN_RGB_LED)
#define ANV_BOARD_LED PIN_RGB_LED /* moves between boards: 48 on most DevKitC-1, 38 on v1.1 */
#else
#define ANV_BOARD_LED 48
#endif
#endif
#ifndef ANV_BOARD_LED_RGB
#define ANV_BOARD_LED_RGB 1
#endif
#ifndef ANV_BOARD_DIP
#define ANV_BOARD_DIP {4, 5, 6, 7}
#endif
#elif defined(CONFIG_IDF_TARGET_ESP32)
/* WROOM DevKits: TX 4 / RX 5 (clear of WROVER PSRAM 16/17 and strapping pin 15), LED on 2.
 * Switches clear of strapping pins (0, 2, 12, 15), flash (6-11) and input-only 34-39. */
#ifndef ANV_BOARD_CAN_TX
#define ANV_BOARD_CAN_TX 4
#endif
#ifndef ANV_BOARD_CAN_RX
#define ANV_BOARD_CAN_RX 5
#endif
#ifndef ANV_BOARD_LED
#define ANV_BOARD_LED 2
#endif
#ifndef ANV_BOARD_LED_RGB
#define ANV_BOARD_LED_RGB 0
#endif
#ifndef ANV_BOARD_DIP
#define ANV_BOARD_DIP {13, 14, 25, 26}
#endif
#else
#error "anv_board_esp32 knows the ESP32 (WROOM) and the ESP32-S3; give the pins with -DANV_BOARD_* for others"
#endif
#ifndef ANV_BOARD_TCAN_STB
#define ANV_BOARD_TCAN_STB (-1)
#endif
#ifndef ANV_BOARD_BITRATE
#define ANV_BOARD_BITRATE 500000u
#endif

void anv_board_pins_default(anv_board_pins_t *p)
{
    memset(p, 0, sizeof *p);
    p->can_tx = ANV_BOARD_CAN_TX;
    p->can_rx = ANV_BOARD_CAN_RX;
    p->tcan_stb = ANV_BOARD_TCAN_STB;
    p->led = ANV_BOARD_LED;
    p->led_rgb = ANV_BOARD_LED_RGB != 0;
    const int dip[] = ANV_BOARD_DIP;
    p->n_dip = (uint8_t)(sizeof dip / sizeof dip[0]);
    if (p->n_dip > ANV_BOARD_MAX_DIP) {
        p->n_dip = ANV_BOARD_MAX_DIP;
    }
    for (uint8_t i = 0; i < p->n_dip; i++) {
        p->dip[i] = dip[i];
    }
    p->bitrate = ANV_BOARD_BITRATE;
}

uint16_t anv_board_read_dip(void *ctx)
{
    const anv_board_pins_t *p = (const anv_board_pins_t *)ctx;
    return p->n_dip == 0u ? 0u : anv_node_esp32_read_dip(p->dip, p->n_dip);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool fail(anv_board_t *b, const char *why)
{
    b->error = why;
    b->ok = false;
    return false;
}

bool anv_board_begin(anv_board_t *b, anv_node_config_t *cfg, const anv_board_pins_t *pins)
{
    memset(b, 0, sizeof *b);
    if (pins != NULL) {
        b->pins = *pins;
    } else {
        anv_board_pins_default(&b->pins);
    }
    const anv_board_pins_t *p = &b->pins;

    if (p->tcan_stb >= 0) { /* transceiver out of standby */
        gpio_reset_pin((gpio_num_t)p->tcan_stb);
        gpio_set_direction((gpio_num_t)p->tcan_stb, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)p->tcan_stb, 0);
    }

    (void)anv_node_nvs_init(&b->nvs, "anv_node");
    cfg->storage = anv_node_nvs_ops();
    cfg->storage_ctx = &b->nvs;
    anv_node_esp32_identity(cfg, &b->nvs);
    if (cfg->ota == NULL) {
        cfg->ota = anv_node_ota_esp32_backend();
    }
    if (cfg->on_reset == NULL) {
        cfg->on_reset = anv_node_esp32_restart;
    }
    cfg->read_dip = anv_board_read_dip;
    cfg->dip_ctx = &b->pins;

    anv_twai_setup(&b->twai, p->can_tx, p->can_rx);
    anv_link_init(&b->link, anv_twai_ops(), &b->twai);
    anv_link_config_t lc;
    anv_link_config_default(&lc);
    lc.bitrate = p->bitrate;
    cfg->link = &b->link;
    if (anv_link_start(&b->link, &lc) != ANV_LINK_OK) {
        return fail(b, "CAN (TWAI) didn't start: check the TX/RX pins");
    }
    if (!anv_node_init(&b->node, cfg)) {
        return fail(b, "node didn't start: type or outputs_off missing, or node_id 255");
    }
    anv_node_begin(&b->node, now_ms());
    b->ok = true;
    b->error = "";
    return true;
}

void anv_board_poll(anv_board_t *b)
{
    if (b->ok) {
        anv_node_poll(&b->node, now_ms());
    }
}

anv_node_t *anv_board_node(anv_board_t *b)
{
    return &b->node;
}

const char *anv_board_error(const anv_board_t *b)
{
    return b->error != NULL ? b->error : "";
}

#else
/* ISO C forbids an empty translation unit. */
typedef int anv_board_esp32_unused_t;
#endif
