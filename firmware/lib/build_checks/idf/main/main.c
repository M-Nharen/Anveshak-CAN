/* The isotp_echo example as a plain ESP-IDF app: proves the libraries build as components.
 * Echoes every ISO-TP transfer back to its sender. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "anv_isotp.h"
#include "anv_link.h"
#include "anv_link_twai.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define CAN_TX 16
#define CAN_RX 15
#else
#define CAN_TX 4
#define CAN_RX 5
#endif

static anv_twai_t twai;
static anv_link_t can_link;
static anv_isotp_t tp;
static anv_isotp_config_t tp_cfg;
static uint8_t rx_buf[ANV_ISOTP_MAX_LEN];
static uint8_t tx_buf[ANV_ISOTP_MAX_LEN];

static uint32_t now_us(void)
{
    return (uint32_t)esp_timer_get_time();
}

static bool send_frame(void *user, const anv_link_frame_t *f)
{
    (void)user;
    return anv_link_send(&can_link, f) == ANV_LINK_OK;
}

static void on_rx(void *user, const uint8_t *data, uint16_t len)
{
    (void)user;
    memcpy(tx_buf, data, len);
    const anv_isotp_result_t r = anv_isotp_send(&tp, tx_buf, len, now_us());
    printf("got %u bytes from %u, echoing: %s\n", len, tp.cfg.peer, anv_isotp_result_name(r));
}

void app_main(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BASE);
    const anv_node_id_t self = (anv_node_id_t)(ANV_NODE_FIRST + mac[5] % (ANV_NODE_LAST - ANV_NODE_FIRST + 1u));

    anv_twai_setup(&twai, CAN_TX, CAN_RX);
    anv_link_init(&can_link, anv_twai_ops(), &twai);
    anv_link_config_t cfg;
    anv_link_config_default(&cfg);
    cfg.n_filters = anv_proto_node_filters(self, cfg.filters);
    const int rc = anv_link_start(&can_link, &cfg);
    printf("node %u, CAN %s at %" PRIu32 " bit/s\n", self, rc == ANV_LINK_OK ? "up" : "FAILED", cfg.bitrate);

    anv_isotp_config_default(&tp_cfg);
    tp_cfg.self = self;
    tp_cfg.peer = ANV_NODE_FIRST;
    tp_cfg.rx_buf = rx_buf;
    tp_cfg.rx_buf_size = sizeof rx_buf;
    tp_cfg.send = send_frame;
    tp_cfg.on_rx = on_rx;
    anv_isotp_init(&tp, &tp_cfg);

    for (;;) {
        anv_link_frame_t f;
        while (anv_link_receive(&can_link, &f)) {
            const anv_node_id_t src = anv_id_src(f.id);
            if (anv_id_port(f.id) == ANV_TRANSFER_PORT && src != tp.cfg.peer && !anv_isotp_tx_busy(&tp) &&
                !anv_isotp_rx_busy(&tp)) {
                tp_cfg.peer = src;
                anv_isotp_init(&tp, &tp_cfg);
            }
            anv_isotp_on_frame(&tp, &f, now_us());
        }
        anv_isotp_poll(&tp, now_us());
        const uint32_t ev = anv_link_poll(&can_link, (uint32_t)(esp_timer_get_time() / 1000));
        if (ev & ANV_LINK_EV_BUS_OFF) {
            printf("CAN bus-off, recovering\n");
        }
        vTaskDelay(1);
    }
}
