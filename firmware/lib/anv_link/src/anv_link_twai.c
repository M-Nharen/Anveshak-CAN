/* Arduino compiles every file in src/, and the Linux build lists its files, so this one guards
 * itself: it only exists where esp_twai does (ESP-IDF 5.5+, Arduino-ESP32 3.3+). */
#if defined(ESP_PLATFORM) && defined(__has_include)
#if __has_include("esp_twai.h")

#include "anv_link_twai.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_twai_onchip.h"
#include "freertos/task.h"

#include "anv_link_twai_filter.h"

static bool IRAM_ATTR on_rx_done(twai_node_handle_t handle, const twai_rx_done_event_data_t *edata, void *user)
{
    (void)edata;
    anv_twai_t *t = (anv_twai_t *)user;
    uint8_t buf[ANV_LINK_MAX_DLC];
    twai_frame_t rx = {.buffer = buf, .buffer_len = sizeof(buf)};
    if (twai_node_receive_from_isr(handle, &rx) != ESP_OK) {
        return false;
    }
    if (!rx.header.ide || rx.header.rtr) {
        return false; /* the v2 protocol is 29-bit data frames only */
    }
    anv_link_frame_t f;
    f.id = rx.header.id & ANV_LINK_EXT_ID_MASK;
    f.len = (uint8_t)(rx.buffer_len > ANV_LINK_MAX_DLC ? ANV_LINK_MAX_DLC : rx.buffer_len);
    memcpy(f.data, buf, f.len);

    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(t->rx_queue, &f, &woken) != pdTRUE) {
        t->rx_overflow++;
    }
    return woken == pdTRUE;
}

static bool IRAM_ATTR on_tx_done(twai_node_handle_t handle, const twai_tx_done_event_data_t *edata, void *user)
{
    (void)handle;
    anv_twai_t *t = (anv_twai_t *)user;
    portENTER_CRITICAL_ISR(&t->lock);
    if (t->tx_in_flight > 0) {
        t->tx_in_flight--;
    }
    portEXIT_CRITICAL_ISR(&t->lock);
    if (edata->is_tx_success) {
        t->tx_ok++;
    } else {
        t->tx_failed++;
    }
    return false;
}

static void clear_in_flight(anv_twai_t *t)
{
    portENTER_CRITICAL(&t->lock);
    t->tx_in_flight = 0;
    portEXIT_CRITICAL(&t->lock);
}

/* The filter can only be changed while the node is disabled. */
static int apply_filters(anv_twai_t *t, const anv_link_filter_t *filters, uint8_t n)
{
    const anv_twai_hw_filter_t hw = anv_twai_hw_filter(filters, n);
    twai_mask_filter_config_t mf = {
        .id = hw.enabled ? hw.id : 0u,
        .mask = hw.enabled ? hw.mask : 0u,
        .is_ext = true,
        .dual_filter = hw.dual,
    };
    return twai_node_config_mask_filter(t->node, 0, &mf) == ESP_OK ? ANV_LINK_OK : ANV_LINK_ERR_BACKEND;
}

/* Creates, configures and enables the controller from the settings kept in `t`. */
static int create_node(anv_twai_t *t)
{
    twai_onchip_node_config_t nc = {
        .io_cfg =
            {
                .tx = (gpio_num_t)t->tx_gpio,
                .rx = (gpio_num_t)t->rx_gpio,
                .quanta_clk_out = GPIO_NUM_NC,
                .bus_off_indicator = GPIO_NUM_NC,
            },
        /* 80 % sample point, as on every other device on the bus (docs/architecture.md). */
        .bit_timing = {.bitrate = t->bitrate, .sp_permill = 800},
        .fail_retry_cnt = -1, /* keep retrying until ACKed; bus-off recovery handles a dead bus */
        .tx_queue_depth = ANV_TWAI_TX_SLOTS,
    };
    if (twai_new_node_onchip(&nc, &t->node) != ESP_OK) {
        t->node = NULL;
        return ANV_LINK_ERR_BACKEND;
    }
    twai_event_callbacks_t cbs = {
        .on_tx_done = on_tx_done,
        .on_rx_done = on_rx_done,
    };
    if (twai_node_register_event_callbacks(t->node, &cbs, t) != ESP_OK ||
        apply_filters(t, t->filters, t->n_filters) != ANV_LINK_OK || twai_node_enable(t->node) != ESP_OK) {
        (void)twai_node_delete(t->node);
        t->node = NULL;
        return ANV_LINK_ERR_BACKEND;
    }
    return ANV_LINK_OK;
}

static void keep_filters(anv_twai_t *t, const anv_link_filter_t *filters, uint8_t n)
{
    t->n_filters = n > ANV_LINK_MAX_FILTERS ? ANV_LINK_MAX_FILTERS : n;
    for (uint8_t i = 0; i < t->n_filters; i++) {
        t->filters[i] = filters[i];
    }
}

static int twai_start(void *ctx, const anv_link_config_t *cfg)
{
    anv_twai_t *t = (anv_twai_t *)ctx;
    if (t->node != NULL) {
        return ANV_LINK_ERR_NOT_READY; /* already started */
    }
    if (t->rx_queue == NULL) {
        t->rx_queue = xQueueCreate(ANV_TWAI_RX_QUEUE_LEN, sizeof(anv_link_frame_t));
        if (t->rx_queue == NULL) {
            return ANV_LINK_ERR_BACKEND;
        }
    }
    xQueueReset(t->rx_queue);
    t->tx_head = 0;
    t->tx_in_flight = 0;
    t->tx_ok = 0;
    t->tx_failed = 0;
    t->rx_overflow = 0;
    t->was_bus_off = false;
    t->restarts = 0;
    t->bitrate = cfg->bitrate;
    keep_filters(t, cfg->filters, cfg->n_filters);
    return create_node(t);
}

static int twai_stop(void *ctx)
{
    anv_twai_t *t = (anv_twai_t *)ctx;
    if (t->node == NULL) {
        return ANV_LINK_OK;
    }
    (void)twai_node_disable(t->node);
    (void)twai_node_delete(t->node);
    t->node = NULL;
    clear_in_flight(t);
    return ANV_LINK_OK;
}

/* Disabling the node drops frames waiting to be sent; filters change rarely (ID assignment),
 * so that is accepted rather than worked around. */
static int twai_set_filters(void *ctx, const anv_link_filter_t *filters, uint8_t n)
{
    anv_twai_t *t = (anv_twai_t *)ctx;
    keep_filters(t, filters, n); /* a controller rebuilt after bus-off gets these too */
    if (t->node == NULL || t->was_bus_off) {
        return ANV_LINK_OK; /* applied when the controller comes back (twai_recover) */
    }
    if (twai_node_disable(t->node) != ESP_OK) {
        return ANV_LINK_ERR_BACKEND;
    }
    clear_in_flight(t);
    const int rc = apply_filters(t, t->filters, t->n_filters);
    if (twai_node_enable(t->node) != ESP_OK) {
        return ANV_LINK_ERR_BACKEND;
    }
    return rc;
}

static int twai_send(void *ctx, const anv_link_frame_t *frame)
{
    anv_twai_t *t = (anv_twai_t *)ctx;
    if (t->node == NULL) {
        return ANV_LINK_ERR_NOT_READY;
    }
    if (t->tx_in_flight >= ANV_TWAI_TX_SLOTS) {
        return ANV_LINK_ERR_TX_FULL;
    }

    const uint8_t slot = t->tx_head;
    twai_frame_t *tf = &t->tx_frames[slot];
    memset(tf, 0, sizeof(*tf));
    memcpy(t->tx_data[slot], frame->data, frame->len);
    tf->header.id = frame->id;
    tf->header.ide = 1;
    tf->buffer = t->tx_data[slot];
    tf->buffer_len = frame->len;

    /* Count it before transmitting: the TX-done interrupt may fire before transmit returns. */
    portENTER_CRITICAL(&t->lock);
    t->tx_in_flight++;
    portEXIT_CRITICAL(&t->lock);
    const esp_err_t err = twai_node_transmit(t->node, tf, 0);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&t->lock);
        t->tx_in_flight--;
        portEXIT_CRITICAL(&t->lock);
        return err == ESP_ERR_TIMEOUT ? ANV_LINK_ERR_TX_FULL : ANV_LINK_ERR_NOT_READY;
    }
    t->tx_head = (uint8_t)((slot + 1u) % ANV_TWAI_TX_SLOTS);
    return ANV_LINK_OK;
}

static bool twai_receive(void *ctx, anv_link_frame_t *frame)
{
    anv_twai_t *t = (anv_twai_t *)ctx;
    return t->rx_queue != NULL && xQueueReceive(t->rx_queue, frame, 0) == pdTRUE;
}

static int twai_get_status(void *ctx, anv_link_hw_status_t *status)
{
    anv_twai_t *t = (anv_twai_t *)ctx;
    twai_node_status_t st;
    if (t->node == NULL) {
        /* A rebuild after bus-off failed: stay bus-off so anv_link keeps asking to recover. */
        status->tec = 255u;
        status->rec = 0u;
        status->state = ANV_LINK_STATE_BUS_OFF;
        t->was_bus_off = true;
        status->rx_overflow = t->rx_overflow;
        status->tx_failed = t->tx_failed;
        return ANV_LINK_OK;
    }
    if (twai_node_get_info(t->node, &st, NULL) != ESP_OK) {
        return ANV_LINK_ERR_BACKEND;
    }
    status->tec = (uint8_t)(st.tx_error_count > 255u ? 255u : st.tx_error_count);
    status->rec = (uint8_t)(st.rx_error_count > 255u ? 255u : st.rx_error_count);
    if (st.state == TWAI_ERROR_BUS_OFF) {
        status->state = ANV_LINK_STATE_BUS_OFF;
        if (!t->was_bus_off) {
            clear_in_flight(t);
            t->bus_off_since = xTaskGetTickCount();
        }
    } else if (st.state == TWAI_ERROR_PASSIVE) {
        status->state = ANV_LINK_STATE_PASSIVE;
    } else {
        status->state = ANV_LINK_STATE_ACTIVE;
    }
    t->was_bus_off = status->state == ANV_LINK_STATE_BUS_OFF;
    status->rx_overflow = t->rx_overflow;
    status->tx_failed = t->tx_failed;
    return ANV_LINK_OK;
}

/* TWAI never leaves bus-off by itself. ESP-IDF's twai_node_recover() is not used: on the
 * classic ESP32 (WROOM) a controller recovered that way could report error-active and still
 * neither send nor acknowledge anything, which then pushed every other node on the bus into
 * bus-off too. Instead the controller is deleted and created again from the settings it was
 * started with, after ANV_TWAI_RESTART_HOLDOFF_MS of bus-off. A fresh controller starts
 * error-active with both counters at zero, like after a reset. */
static int twai_recover(void *ctx)
{
    anv_twai_t *t = (anv_twai_t *)ctx;
    if (t->node != NULL) {
        twai_node_status_t st;
        if (twai_node_get_info(t->node, &st, NULL) != ESP_OK || st.state != TWAI_ERROR_BUS_OFF) {
            return ANV_LINK_OK; /* not bus-off (any more): nothing to do */
        }
        if ((TickType_t)(xTaskGetTickCount() - t->bus_off_since) < pdMS_TO_TICKS(ANV_TWAI_RESTART_HOLDOFF_MS)) {
            return ANV_LINK_OK; /* too soon; anv_link asks again after recover_retry_ms */
        }
        if (twai_node_delete(t->node) != ESP_OK) {
            return ANV_LINK_ERR_BACKEND;
        }
        t->node = NULL;
    }
    clear_in_flight(t);
    t->tx_head = 0;
    const int rc = create_node(t);
    if (rc == ANV_LINK_OK) {
        t->restarts++;
        t->was_bus_off = false;
    }
    return rc;
}

static const anv_link_ops_t s_ops = {
    .start = twai_start,
    .stop = twai_stop,
    .set_filters = twai_set_filters,
    .send = twai_send,
    .receive = twai_receive,
    .get_status = twai_get_status,
    .recover = twai_recover,
};

const anv_link_ops_t *anv_twai_ops(void)
{
    return &s_ops;
}

void anv_twai_setup(anv_twai_t *twai, int tx_gpio, int rx_gpio)
{
    memset(twai, 0, sizeof(*twai));
    portMUX_INITIALIZE(&twai->lock);
    twai->tx_gpio = tx_gpio;
    twai->rx_gpio = rx_gpio;
}

bool anv_twai_get_info(anv_twai_t *twai, twai_node_status_t *status, twai_node_record_t *record)
{
    return twai->node != NULL && twai_node_get_info(twai->node, status, record) == ESP_OK;
}

#endif /* __has_include("esp_twai.h") */
#endif /* ESP_PLATFORM */
