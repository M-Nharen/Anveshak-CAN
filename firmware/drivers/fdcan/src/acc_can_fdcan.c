#include "acc_can_fdcan.h"
#include "acc_can_fdcan_timing.h"

#include <string.h>

static int fdcan_init(void *vctx, uint32_t bitrate_bps, const acc_can_filter_t *filters, size_t n_filters)
{
    acc_can_fdcan_ctx_t *ctx = (acc_can_fdcan_ctx_t *)vctx;
    (void)bitrate_bps; /* fixed at compile time via ACC_CAN_FDCAN_CLOCK_HZ; see acc_can_fdcan_timing.h */

    if (n_filters > ACC_CAN_MAX_FILTERS) {
        return ACC_CAN_ERR_INVALID_ARG;
    }

    FDCAN_HandleTypeDef *hfdcan = ctx->hfdcan;
    hfdcan->Init.ClockDivider = FDCAN_CLOCK_DIV1;
    hfdcan->Init.FrameFormat = FDCAN_FRAME_CLASSIC;
    hfdcan->Init.Mode = FDCAN_MODE_NORMAL;
    hfdcan->Init.AutoRetransmission = ENABLE; /* NART off, per docs/architecture.md */
    hfdcan->Init.TransmitPause = DISABLE;
    hfdcan->Init.ProtocolException = DISABLE;
    hfdcan->Init.NominalPrescaler = ACC_CAN_FDCAN_PRESCALER;
    hfdcan->Init.NominalSyncJumpWidth = 1u;
    hfdcan->Init.NominalTimeSeg1 = ACC_CAN_FDCAN_BS1_TQ;
    hfdcan->Init.NominalTimeSeg2 = ACC_CAN_FDCAN_BS2_TQ;
    /* Data phase fields are unused in classic mode (no bit-rate switching) but HAL still range
     * checks them at Init time, so mirror the nominal timing into them. */
    hfdcan->Init.DataPrescaler = ACC_CAN_FDCAN_PRESCALER;
    hfdcan->Init.DataSyncJumpWidth = 1u;
    hfdcan->Init.DataTimeSeg1 = ACC_CAN_FDCAN_BS1_TQ;
    hfdcan->Init.DataTimeSeg2 = ACC_CAN_FDCAN_BS2_TQ;
    hfdcan->Init.StdFiltersNbr = (uint32_t)n_filters;
    hfdcan->Init.ExtFiltersNbr = 0u;
    hfdcan->Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;

    if (HAL_FDCAN_Init(hfdcan) != HAL_OK) {
        return ACC_CAN_ERR_BACKEND;
    }

    for (size_t i = 0; i < n_filters; i++) {
        FDCAN_FilterTypeDef filter = {0};
        filter.IdType = FDCAN_STANDARD_ID;
        filter.FilterIndex = (uint32_t)i;
        filter.FilterType = FDCAN_FILTER_MASK;
        filter.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
        filter.FilterID1 = filters[i].id;  /* classic filter: FilterID1 = filter, FilterID2 = mask */
        filter.FilterID2 = filters[i].mask;
        if (HAL_FDCAN_ConfigFilter(hfdcan, &filter) != HAL_OK) {
            return ACC_CAN_ERR_BACKEND;
        }
    }

    /* n_filters == 0 means "accept everything" (acc_can.h): let non-matching standard frames
     * through too instead of configuring a dummy filter element. Extended frames and remote
     * frames never appear on this bus (classic CAN 2.0A, 11-bit IDs only), so always reject
     * those regardless of n_filters. */
    uint32_t non_matching_std = (n_filters == 0u) ? FDCAN_ACCEPT_IN_RX_FIFO0 : FDCAN_REJECT;
    if (HAL_FDCAN_ConfigGlobalFilter(hfdcan, non_matching_std, FDCAN_REJECT, FDCAN_REJECT_REMOTE,
                                      FDCAN_REJECT_REMOTE) != HAL_OK) {
        return ACC_CAN_ERR_BACKEND;
    }

    anv_frame_ring_init(&ctx->rx_ring, ctx->rx_storage, ACC_CAN_FDCAN_RX_CAPACITY);
    memset(&ctx->error_state, 0, sizeof(ctx->error_state));

    if (HAL_FDCAN_ActivateNotification(hfdcan, FDCAN_IT_RX_FIFO0_NEW_MESSAGE | FDCAN_IT_BUS_OFF |
                                                    FDCAN_IT_ERROR_PASSIVE | FDCAN_IT_ERROR_WARNING,
                                        0u) != HAL_OK) {
        return ACC_CAN_ERR_BACKEND;
    }
    if (HAL_FDCAN_Start(hfdcan) != HAL_OK) {
        return ACC_CAN_ERR_BACKEND;
    }

    return ACC_CAN_OK;
}

static int fdcan_send(void *vctx, const acc_can_frame_t *frame, acc_can_priority_t prio)
{
    (void)prio; /* software priority is already handled by anv_txqueue before this is called */
    acc_can_fdcan_ctx_t *ctx = (acc_can_fdcan_ctx_t *)vctx;

    if (ctx->error_state.state == ACC_CAN_BUS_OFF) {
        return ACC_CAN_ERR_NOT_READY;
    }
    if (HAL_FDCAN_GetTxFifoFreeLevel(ctx->hfdcan) == 0u) {
        return ACC_CAN_ERR_TX_FULL;
    }

    FDCAN_TxHeaderTypeDef header = {0};
    header.Identifier = frame->id;
    header.IdType = FDCAN_STANDARD_ID;
    header.TxFrameType = FDCAN_DATA_FRAME;
    /* FDCAN_DLC_BYTES_n == n for every classic-frame length (0-8), so the raw byte count can
     * be used directly without a lookup table. */
    header.DataLength = frame->dlc;
    header.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    header.BitRateSwitch = FDCAN_BRS_OFF;
    header.FDFormat = FDCAN_CLASSIC_CAN;
    header.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    header.MessageMarker = 0u;

    /* HAL_FDCAN_AddMessageToTxFifoQ() takes a non-const uint8_t* even though it only reads it. */
    if (HAL_FDCAN_AddMessageToTxFifoQ(ctx->hfdcan, &header, (uint8_t *)(uintptr_t)frame->data) != HAL_OK) {
        return ACC_CAN_ERR_BACKEND;
    }
    return ACC_CAN_OK;
}

static bool fdcan_receive(void *vctx, acc_can_frame_t *frame)
{
    acc_can_fdcan_ctx_t *ctx = (acc_can_fdcan_ctx_t *)vctx;
    return anv_frame_ring_pop(&ctx->rx_ring, frame);
}

static void refresh_error_state(acc_can_fdcan_ctx_t *ctx)
{
    FDCAN_ProtocolStatusTypeDef proto;
    FDCAN_ErrorCountersTypeDef counters;
    (void)HAL_FDCAN_GetProtocolStatus(ctx->hfdcan, &proto);
    (void)HAL_FDCAN_GetErrorCounters(ctx->hfdcan, &counters);

    bool was_bus_off = (ctx->error_state.state == ACC_CAN_BUS_OFF);

    /* Reading the protocol status clears its last-error-code field, so each read reports what
     * happened since the previous read (called every tick and from the error interrupt). Two
     * errors between reads count once: it shows which kind dominates, not an exact total. */
    uint32_t *counts = ctx->error_state.error_type_counts;
    switch (proto.LastErrorCode) {
    case FDCAN_PROTOCOL_ERROR_STUFF:
        counts[ACC_CAN_ERRTYPE_STUFF]++;
        break;
    case FDCAN_PROTOCOL_ERROR_FORM:
        counts[ACC_CAN_ERRTYPE_FORM]++;
        break;
    case FDCAN_PROTOCOL_ERROR_ACK:
        counts[ACC_CAN_ERRTYPE_ACK]++;
        break;
    case FDCAN_PROTOCOL_ERROR_BIT1:
    case FDCAN_PROTOCOL_ERROR_BIT0:
        counts[ACC_CAN_ERRTYPE_BIT]++;
        break;
    case FDCAN_PROTOCOL_ERROR_CRC:
        counts[ACC_CAN_ERRTYPE_CRC]++;
        break;
    default:
        break;
    }

    ctx->error_state.tec = (uint8_t)counters.TxErrorCnt;
    ctx->error_state.rec = (uint8_t)counters.RxErrorCnt;

    if (proto.BusOff != 0u) {
        ctx->error_state.state = ACC_CAN_BUS_OFF;
        if (!was_bus_off) {
            ctx->error_state.bus_off_count++;
        }
    } else if (proto.ErrorPassive != 0u) {
        ctx->error_state.state = ACC_CAN_ERROR_PASSIVE;
    } else {
        ctx->error_state.state = ACC_CAN_ERROR_ACTIVE;
    }
}

static int fdcan_get_error_state(void *vctx, acc_can_error_state_t *state)
{
    acc_can_fdcan_ctx_t *ctx = (acc_can_fdcan_ctx_t *)vctx;
    refresh_error_state(ctx);
    *state = ctx->error_state;
    return ACC_CAN_OK;
}

/* FDCAN does not leave bus-off by itself (docs/architecture.md): the hardware sets CCCR.INIT
 * on bus-off, and HAL_FDCAN_Start() is what clears it, so calling it again is the recovery. */
static int fdcan_start_recovery(void *vctx)
{
    acc_can_fdcan_ctx_t *ctx = (acc_can_fdcan_ctx_t *)vctx;
    if (HAL_FDCAN_Start(ctx->hfdcan) != HAL_OK) {
        return ACC_CAN_ERR_BACKEND;
    }
    return ACC_CAN_OK;
}

acc_can_driver_t acc_can_fdcan_make_driver(acc_can_fdcan_ctx_t *ctx, FDCAN_HandleTypeDef *hfdcan)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->hfdcan = hfdcan;

    acc_can_driver_t driver;
    driver.ctx = ctx;
    driver.init = fdcan_init;
    driver.send = fdcan_send;
    driver.receive = fdcan_receive;
    driver.get_error_state = fdcan_get_error_state;
    driver.start_recovery = fdcan_start_recovery;
    return driver;
}

void acc_can_fdcan_isr_rx_fifo0(acc_can_fdcan_ctx_t *ctx)
{
    FDCAN_RxHeaderTypeDef header;
    acc_can_frame_t frame;

    while (HAL_FDCAN_GetRxFifoFillLevel(ctx->hfdcan, FDCAN_RX_FIFO0) > 0u) {
        if (HAL_FDCAN_GetRxMessage(ctx->hfdcan, FDCAN_RX_FIFO0, &header, frame.data) != HAL_OK) {
            return;
        }
        frame.id = (uint16_t)header.Identifier;
        frame.dlc = (uint8_t)header.DataLength; /* FDCAN_DLC_BYTES_n == n for a classic frame */
        (void)anv_frame_ring_push(&ctx->rx_ring, &frame);
    }
}

void acc_can_fdcan_isr_error(acc_can_fdcan_ctx_t *ctx)
{
    refresh_error_state(ctx);
}
