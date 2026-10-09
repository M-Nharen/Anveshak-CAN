#include "acc_can_twai.h"

#include <string.h>

/* Classic (SJA1000-compatible) dual filter mode packs two independent 16-bit sub-filters into
 * the 32-bit acceptance_code/acceptance_mask registers: each half holds the 11-bit standard ID
 * in bits 15..5, the RTR bit in bit 4, and 4 bits of the first data byte in bits 3..0 that this
 * driver never filters on (always "don't care"). Filter 1 occupies the upper 16 bits, filter 2
 * the lower 16 bits. acceptance_mask is 1 = don't care in hardware (docs/architecture.md,
 * "ESP32-S3 nodes"), the opposite of acc_can_filter_t's 1 = must match, so the mask is
 * inverted here before it is packed. */
static uint16_t dual_filter_half(uint16_t id, uint16_t mask, bool is_mask)
{
    if (!is_mask) {
        return (uint16_t)((id & 0x7FFu) << 5);
    }
    uint16_t dont_care_id_bits = (uint16_t)(~mask & 0x7FFu);
    return (uint16_t)((dont_care_id_bits << 5) | 0x1Fu); /* RTR + 4 data bits: always don't care */
}

static void build_dual_filter(const acc_can_filter_t *filters, size_t n_filters, twai_filter_config_t *out)
{
    acc_can_filter_t f0 = {0, 0}; /* {id 0, mask 0} == accept everything in our convention */
    acc_can_filter_t f1 = {0, 0};

    if (n_filters >= 1u) {
        f0 = filters[0];
        f1 = filters[0];
    }
    if (n_filters >= 2u) {
        f1 = filters[1];
    }

    uint32_t code_hi = dual_filter_half(f0.id, f0.mask, false);
    uint32_t mask_hi = dual_filter_half(f0.id, f0.mask, true);
    uint32_t code_lo = dual_filter_half(f1.id, f1.mask, false);
    uint32_t mask_lo = dual_filter_half(f1.id, f1.mask, true);

    out->acceptance_code = (code_hi << 16) | code_lo;
    out->acceptance_mask = (mask_hi << 16) | mask_lo;
    out->single_filter = false; /* dual filter mode */
}

static void rx_task_fn(void *arg)
{
    acc_can_twai_ctx_t *ctx = (acc_can_twai_ctx_t *)arg;
    twai_message_t msg;

    for (;;) {
        if (twai_receive(&msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        if (msg.rtr || msg.extd || msg.data_length_code > ACC_CAN_MAX_DLC) {
            continue; /* this bus never carries remote or extended-ID frames */
        }
        acc_can_frame_t frame;
        frame.id = (uint16_t)msg.identifier;
        frame.dlc = msg.data_length_code;
        memcpy(frame.data, msg.data, msg.data_length_code);
        (void)anv_frame_ring_push(&ctx->rx_ring, &frame);
    }
}

static int twai_drv_init(void *vctx, uint32_t bitrate_bps, const acc_can_filter_t *filters, size_t n_filters)
{
    acc_can_twai_ctx_t *ctx = (acc_can_twai_ctx_t *)vctx;
    (void)bitrate_bps; /* fixed at compile time; see the timing config below */

    if (n_filters > ACC_CAN_MAX_FILTERS) {
        return ACC_CAN_ERR_INVALID_ARG;
    }

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(ctx->tx_io, ctx->rx_io, TWAI_MODE_NORMAL);
    g_config.tx_queue_len = 16;
    g_config.rx_queue_len = 32; /* sized for bursts, docs/architecture.md */

    /* ESP-IDF's ready-made 500 kbps timing is prescaler 8, seg1 15, seg2 4, SJW 3: 20 TQ per
     * bit, 80% sample point (docs/architecture.md, "ESP32-S3 nodes") -- exactly what
     * TWAI_TIMING_CONFIG_500KBITS() gives. The struct's exact fields have changed across
     * ESP-IDF versions (e.g. v5.3 has neither prop_seg nor ssp_offset), so the official macro
     * is used rather than a hand-written literal. */
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();

    twai_filter_config_t f_config;
    build_dual_filter(filters, n_filters, &f_config);

    if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK) {
        return ACC_CAN_ERR_BACKEND;
    }
    if (twai_start() != ESP_OK) {
        return ACC_CAN_ERR_BACKEND;
    }

    anv_frame_ring_init(&ctx->rx_ring, ctx->rx_storage, ACC_CAN_TWAI_RX_CAPACITY);
    memset(&ctx->error_state, 0, sizeof(ctx->error_state));

    /* RX task pinned to core 1, Wi-Fi/Bluetooth left on core 0 (docs/architecture.md). */
    BaseType_t created = xTaskCreatePinnedToCore(rx_task_fn, "acc_can_twai_rx", ACC_CAN_TWAI_RX_TASK_STACK_WORDS, ctx,
                                                  ACC_CAN_TWAI_RX_TASK_PRIORITY, &ctx->rx_task,
                                                  ACC_CAN_TWAI_RX_TASK_CORE);
    if (created != pdPASS) {
        return ACC_CAN_ERR_BACKEND;
    }

    return ACC_CAN_OK;
}

static int twai_drv_send(void *vctx, const acc_can_frame_t *frame, acc_can_priority_t prio)
{
    (void)vctx;
    (void)prio; /* software priority is already handled by anv_txqueue before this is called */

    twai_message_t msg = {0};
    msg.identifier = frame->id;
    msg.data_length_code = frame->dlc;
    memcpy(msg.data, frame->data, frame->dlc);

    esp_err_t err = twai_transmit(&msg, 0); /* never block: anv_txqueue already queued it */
    switch (err) {
    case ESP_OK:
        return ACC_CAN_OK;
    case ESP_ERR_TIMEOUT:
        return ACC_CAN_ERR_TX_FULL;
    case ESP_ERR_INVALID_STATE:
        return ACC_CAN_ERR_NOT_READY; /* e.g. bus-off */
    default:
        return ACC_CAN_ERR_BACKEND;
    }
}

static bool twai_drv_receive(void *vctx, acc_can_frame_t *frame)
{
    acc_can_twai_ctx_t *ctx = (acc_can_twai_ctx_t *)vctx;
    return anv_frame_ring_pop(&ctx->rx_ring, frame);
}

static int twai_drv_get_error_state(void *vctx, acc_can_error_state_t *state)
{
    acc_can_twai_ctx_t *ctx = (acc_can_twai_ctx_t *)vctx;
    twai_status_info_t status;
    if (twai_get_status_info(&status) != ESP_OK) {
        return ACC_CAN_ERR_BACKEND;
    }

    bool was_bus_off = (ctx->error_state.state == ACC_CAN_BUS_OFF);
    ctx->error_state.tec = (uint8_t)(status.tx_error_counter > 255u ? 255u : status.tx_error_counter);
    ctx->error_state.rec = (uint8_t)(status.rx_error_counter > 255u ? 255u : status.rx_error_counter);

    /* BUS_OFF, RECOVERING and STOPPED (the state recovery leaves the controller in, per
     * ESP-IDF's twai_initiate_recovery() docs) are all "can't trust the bus yet" from the
     * core's point of view, and all three keep start_recovery() being called every tick until
     * twai_start() succeeds and the state comes back RUNNING. */
    if (status.state != TWAI_STATE_RUNNING) {
        ctx->error_state.state = ACC_CAN_BUS_OFF;
        if (!was_bus_off) {
            ctx->error_state.bus_off_count++;
        }
    } else if (ctx->error_state.rec >= 128u || ctx->error_state.tec >= 128u) {
        ctx->error_state.state = ACC_CAN_ERROR_PASSIVE;
    } else {
        ctx->error_state.state = ACC_CAN_ERROR_ACTIVE;
    }

    *state = ctx->error_state;
    return ACC_CAN_OK;
}

/* TWAI does not leave bus-off by itself (docs/architecture.md): kick off recovery, then once
 * it completes (the controller lands in STOPPED), restart it. */
static int twai_drv_start_recovery(void *vctx)
{
    (void)vctx;
    twai_status_info_t status;
    if (twai_get_status_info(&status) != ESP_OK) {
        return ACC_CAN_ERR_BACKEND;
    }
    if (status.state == TWAI_STATE_BUS_OFF) {
        esp_err_t err = twai_initiate_recovery();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return ACC_CAN_ERR_BACKEND;
        }
    } else if (status.state == TWAI_STATE_STOPPED) {
        if (twai_start() != ESP_OK) {
            return ACC_CAN_ERR_BACKEND;
        }
    }
    return ACC_CAN_OK;
}

acc_can_driver_t acc_can_twai_make_driver(acc_can_twai_ctx_t *ctx, gpio_num_t tx_io, gpio_num_t rx_io)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->tx_io = tx_io;
    ctx->rx_io = rx_io;

    acc_can_driver_t driver;
    driver.ctx = ctx;
    driver.init = twai_drv_init;
    driver.send = twai_drv_send;
    driver.receive = twai_drv_receive;
    driver.get_error_state = twai_drv_get_error_state;
    driver.start_recovery = twai_drv_start_recovery;
    return driver;
}
