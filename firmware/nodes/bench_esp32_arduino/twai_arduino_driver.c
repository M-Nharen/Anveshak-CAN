/* acc_can_driver_t over esp_twai. Hardware acceptance filters are not used: bench traffic is a few
 * hundred frames per second, and anv_node_on_rx_frame() already ignores frames addressed to other
 * nodes. Add twai_node_config_mask_filter() here before putting these boards on the full rover. */
#include "twai_arduino_driver.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_twai_onchip.h"

static bool IRAM_ATTR on_rx_done(twai_node_handle_t handle, const twai_rx_done_event_data_t *edata, void *user)
{
    (void)edata;
    twai_ard_ctx_t *ctx = (twai_ard_ctx_t *)user;
    uint8_t buf[8];
    twai_frame_t rx = {.buffer = buf, .buffer_len = sizeof(buf)};
    if (twai_node_receive_from_isr(handle, &rx) != ESP_OK) {
        return false;
    }
    if (rx.header.ide || rx.header.rtr) {
        return false; /* CAN 2.0A data frames only */
    }
    acc_can_frame_t f;
    f.id = (uint16_t)(rx.header.id & 0x7FFu);
    f.dlc = (uint8_t)(rx.buffer_len > 8 ? 8 : rx.buffer_len);
    memcpy(f.data, buf, f.dlc);

    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(ctx->rx_queue, &f, &woken) != pdTRUE) {
        ctx->rx_dropped++;
    }
    return woken == pdTRUE;
}

static bool IRAM_ATTR on_tx_done(twai_node_handle_t handle, const twai_tx_done_event_data_t *edata, void *user)
{
    (void)handle;
    twai_ard_ctx_t *ctx = (twai_ard_ctx_t *)user;
    portENTER_CRITICAL_ISR(&ctx->lock);
    if (ctx->tx_in_flight > 0) {
        ctx->tx_in_flight--;
    }
    portEXIT_CRITICAL_ISR(&ctx->lock);
    if (edata->is_tx_success) {
        ctx->tx_ok_count++;
    }
    return false;
}

static int drv_init(void *vctx, uint32_t bitrate_bps, const acc_can_filter_t *filters, size_t n_filters)
{
    (void)filters;
    (void)n_filters;
    twai_ard_ctx_t *ctx = (twai_ard_ctx_t *)vctx;

    ctx->rx_queue = xQueueCreate(TWAI_ARD_RX_QUEUE_LEN, sizeof(acc_can_frame_t));
    if (ctx->rx_queue == NULL) {
        return ACC_CAN_ERR_BACKEND;
    }

    twai_onchip_node_config_t cfg = {
        .io_cfg =
            {
                .tx = (gpio_num_t)ctx->tx_gpio,
                .rx = (gpio_num_t)ctx->rx_gpio,
                .quanta_clk_out = GPIO_NUM_NC,
                .bus_off_indicator = GPIO_NUM_NC,
            },
        /* 80 % sample point, as on every other device on the bus (docs/architecture.md). */
        .bit_timing = {.bitrate = bitrate_bps, .sp_permill = 800},
        .fail_retry_cnt = -1, /* keep retrying until ACKed; bus-off recovery handles a dead bus */
        .tx_queue_depth = TWAI_ARD_TX_SLOTS,
    };
    if (twai_new_node_onchip(&cfg, &ctx->node) != ESP_OK) {
        return ACC_CAN_ERR_BACKEND;
    }
    twai_event_callbacks_t cbs = {
        .on_tx_done = on_tx_done,
        .on_rx_done = on_rx_done,
    };
    if (twai_node_register_event_callbacks(ctx->node, &cbs, ctx) != ESP_OK) {
        return ACC_CAN_ERR_BACKEND;
    }
    if (twai_node_enable(ctx->node) != ESP_OK) {
        return ACC_CAN_ERR_BACKEND;
    }
    return ACC_CAN_OK;
}

static int drv_send(void *vctx, const acc_can_frame_t *frame, acc_can_priority_t prio)
{
    (void)prio; /* anv_txqueue already orders frames by priority */
    twai_ard_ctx_t *ctx = (twai_ard_ctx_t *)vctx;
    if (ctx->node == NULL) {
        return ACC_CAN_ERR_NOT_READY;
    }
    if (ctx->tx_in_flight >= TWAI_ARD_TX_SLOTS) {
        return ACC_CAN_ERR_TX_FULL; /* stays in anv_txqueue, retried next drain */
    }

    const uint8_t slot = ctx->tx_head;
    twai_frame_t *tf = &ctx->tx_frames[slot];
    memset(tf, 0, sizeof(*tf));
    memcpy(ctx->tx_data[slot], frame->data, frame->dlc);
    tf->header.id = frame->id;
    tf->buffer = ctx->tx_data[slot];
    tf->buffer_len = frame->dlc;

    /* Count it before transmitting: the TX-done interrupt may fire before transmit returns. */
    portENTER_CRITICAL(&ctx->lock);
    ctx->tx_in_flight++;
    portEXIT_CRITICAL(&ctx->lock);
    esp_err_t err = twai_node_transmit(ctx->node, tf, 0);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&ctx->lock);
        ctx->tx_in_flight--;
        portEXIT_CRITICAL(&ctx->lock);
        return err == ESP_ERR_TIMEOUT ? ACC_CAN_ERR_TX_FULL : ACC_CAN_ERR_NOT_READY;
    }
    ctx->tx_head = (uint8_t)((slot + 1u) % TWAI_ARD_TX_SLOTS);
    return ACC_CAN_OK;
}

static bool drv_receive(void *vctx, acc_can_frame_t *frame)
{
    twai_ard_ctx_t *ctx = (twai_ard_ctx_t *)vctx;
    return ctx->rx_queue != NULL && xQueueReceive(ctx->rx_queue, frame, 0) == pdTRUE;
}

static int drv_get_error_state(void *vctx, acc_can_error_state_t *state)
{
    twai_ard_ctx_t *ctx = (twai_ard_ctx_t *)vctx;
    twai_node_status_t st;
    if (ctx->node == NULL || twai_node_get_info(ctx->node, &st, NULL) != ESP_OK) {
        return ACC_CAN_ERR_BACKEND;
    }
    const bool was_bus_off = ctx->error_state.state == ACC_CAN_BUS_OFF;
    ctx->error_state.tec = (uint8_t)(st.tx_error_count > 255u ? 255u : st.tx_error_count);
    ctx->error_state.rec = (uint8_t)(st.rx_error_count > 255u ? 255u : st.rx_error_count);
    if (st.state == TWAI_ERROR_BUS_OFF) {
        ctx->error_state.state = ACC_CAN_BUS_OFF;
        if (!was_bus_off) {
            ctx->error_state.bus_off_count++;
            portENTER_CRITICAL(&ctx->lock);
            ctx->tx_in_flight = 0; /* the driver drops queued frames on bus-off */
            portEXIT_CRITICAL(&ctx->lock);
        }
    } else if (st.state == TWAI_ERROR_PASSIVE) {
        ctx->error_state.state = ACC_CAN_ERROR_PASSIVE;
    } else {
        ctx->error_state.state = ACC_CAN_ERROR_ACTIVE;
    }
    *state = ctx->error_state;
    return ACC_CAN_OK;
}

/* TWAI never leaves bus-off by itself. twai_node_recover() starts the 128 x 11 recessive-bit
 * wait; calling it again while that is running just returns an error, which is fine. */
static int drv_start_recovery(void *vctx)
{
    twai_ard_ctx_t *ctx = (twai_ard_ctx_t *)vctx;
    if (ctx->node == NULL) {
        return ACC_CAN_ERR_NOT_READY;
    }
    (void)twai_node_recover(ctx->node);
    return ACC_CAN_OK;
}

acc_can_driver_t twai_ard_make_driver(twai_ard_ctx_t *ctx, int tx_gpio, int rx_gpio)
{
    memset(ctx, 0, sizeof(*ctx));
    portMUX_INITIALIZE(&ctx->lock);
    ctx->tx_gpio = tx_gpio;
    ctx->rx_gpio = rx_gpio;
    acc_can_driver_t d = {
        .ctx = ctx,
        .init = drv_init,
        .send = drv_send,
        .receive = drv_receive,
        .get_error_state = drv_get_error_state,
        .start_recovery = drv_start_recovery,
    };
    return d;
}

bool twai_ard_get_info(twai_ard_ctx_t *ctx, twai_node_status_t *status, twai_node_record_t *record)
{
    return ctx->node != NULL && twai_node_get_info(ctx->node, status, record) == ESP_OK;
}
