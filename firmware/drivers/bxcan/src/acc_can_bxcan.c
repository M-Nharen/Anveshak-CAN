#include "acc_can_bxcan.h"
#include "acc_can_bxcan_timing.h"

#include <string.h>

/* STD ID, IDE=0 (standard), RTR=0 (data frame) all three packed into the 32-bit scale filter
 * register the same way HAL_CAN expects it: ID in bits 31..21, IDE in bit 2, RTR in bit 1. The
 * mask always forces IDE=0 and RTR=0 as part of the match (this bus only ever carries standard
 * 11-bit data frames), on top of whatever ID bits the caller's mask covers. */
static uint32_t pack_filter_reg(uint16_t id, uint16_t mask, bool is_mask)
{
    uint32_t id_bits = ((uint32_t)id & 0x7FFu) << 21;
    if (!is_mask) {
        return id_bits;
    }
    uint32_t mask_bits = ((uint32_t)mask & 0x7FFu) << 21;
    return mask_bits | (1u << 2) /* IDE must match */ | (1u << 1) /* RTR must match */;
}

static int bxcan_init(void *vctx, uint32_t bitrate_bps, const acc_can_filter_t *filters, size_t n_filters)
{
    acc_can_bxcan_ctx_t *ctx = (acc_can_bxcan_ctx_t *)vctx;
    (void)bitrate_bps; /* fixed at compile time via ACC_CAN_BXCAN_CLOCK_HZ; see acc_can_bxcan_timing.h */

    if (n_filters > ACC_CAN_MAX_FILTERS) {
        return ACC_CAN_ERR_INVALID_ARG;
    }

    CAN_HandleTypeDef *hcan = ctx->hcan;
    hcan->Init.Prescaler = ACC_CAN_BXCAN_PRESCALER;
    hcan->Init.Mode = CAN_MODE_NORMAL;
    hcan->Init.SyncJumpWidth = CAN_SJW_1TQ;
    hcan->Init.TimeSeg1 = ACC_CAN_BXCAN_HAL_BS1;
    hcan->Init.TimeSeg2 = ACC_CAN_BXCAN_HAL_BS2;
    hcan->Init.TimeTriggeredMode = DISABLE;
    hcan->Init.AutoBusOff = ENABLE;         /* ABOM on */
    hcan->Init.AutoWakeUp = DISABLE;
    hcan->Init.AutoRetransmission = ENABLE; /* NART off */
    hcan->Init.ReceiveFifoLocked = DISABLE;
    hcan->Init.TransmitFifoPriority = ENABLE; /* TXFP on */

    if (HAL_CAN_Init(hcan) != HAL_OK) {
        return ACC_CAN_ERR_BACKEND;
    }

    CAN_FilterTypeDef filter = {0};
    filter.FilterFIFOAssignment = CAN_FILTER_FIFO0;
    filter.FilterMode = CAN_FILTERMODE_IDMASK;
    filter.FilterScale = CAN_FILTERSCALE_32BIT;
    filter.FilterActivation = CAN_FILTER_ENABLE;
    filter.SlaveStartFilterBank = 14u; /* meaningless on a single (non-dual) CAN instance */

    if (n_filters == 0u) {
        /* Accept everything: ID 0, mask 0 (only IDE/RTR still enforced above). */
        filter.FilterBank = 0u;
        filter.FilterIdHigh = (uint16_t)(pack_filter_reg(0u, 0u, false) >> 16);
        filter.FilterIdLow = (uint16_t)(pack_filter_reg(0u, 0u, false) & 0xFFFFu);
        filter.FilterMaskIdHigh = (uint16_t)(pack_filter_reg(0u, 0u, true) >> 16);
        filter.FilterMaskIdLow = (uint16_t)(pack_filter_reg(0u, 0u, true) & 0xFFFFu);
        if (HAL_CAN_ConfigFilter(hcan, &filter) != HAL_OK) {
            return ACC_CAN_ERR_BACKEND;
        }
    } else {
        for (size_t i = 0; i < n_filters; i++) {
            uint32_t id_reg = pack_filter_reg(filters[i].id, filters[i].mask, false);
            uint32_t mask_reg = pack_filter_reg(filters[i].id, filters[i].mask, true);
            filter.FilterBank = (uint32_t)i;
            filter.FilterIdHigh = (uint16_t)(id_reg >> 16);
            filter.FilterIdLow = (uint16_t)(id_reg & 0xFFFFu);
            filter.FilterMaskIdHigh = (uint16_t)(mask_reg >> 16);
            filter.FilterMaskIdLow = (uint16_t)(mask_reg & 0xFFFFu);
            if (HAL_CAN_ConfigFilter(hcan, &filter) != HAL_OK) {
                return ACC_CAN_ERR_BACKEND;
            }
        }
    }

    anv_frame_ring_init(&ctx->rx_ring, ctx->rx_storage, ACC_CAN_BXCAN_RX_CAPACITY);
    memset(&ctx->error_state, 0, sizeof(ctx->error_state));

    if (HAL_CAN_ActivateNotification(hcan, CAN_IT_RX_FIFO0_MSG_PENDING | CAN_IT_BUSOFF | CAN_IT_ERROR) != HAL_OK) {
        return ACC_CAN_ERR_BACKEND;
    }
    if (HAL_CAN_Start(hcan) != HAL_OK) {
        return ACC_CAN_ERR_BACKEND;
    }

    return ACC_CAN_OK;
}

static int bxcan_send(void *vctx, const acc_can_frame_t *frame, acc_can_priority_t prio)
{
    (void)prio; /* software priority is already handled by anv_txqueue before this is called */
    acc_can_bxcan_ctx_t *ctx = (acc_can_bxcan_ctx_t *)vctx;

    if (ctx->error_state.state == ACC_CAN_BUS_OFF) {
        return ACC_CAN_ERR_NOT_READY;
    }
    if (HAL_CAN_GetTxMailboxesFreeLevel(ctx->hcan) == 0u) {
        return ACC_CAN_ERR_TX_FULL;
    }

    CAN_TxHeaderTypeDef header = {0};
    header.StdId = frame->id;
    header.IDE = CAN_ID_STD;
    header.RTR = CAN_RTR_DATA;
    header.DLC = frame->dlc;
    header.TransmitGlobalTime = DISABLE;

    uint32_t mailbox;
    /* HAL_CAN_AddTxMessage() takes a non-const uint8_t* even though it only reads the data. */
    if (HAL_CAN_AddTxMessage(ctx->hcan, &header, (uint8_t *)(uintptr_t)frame->data, &mailbox) != HAL_OK) {
        return ACC_CAN_ERR_BACKEND;
    }
    return ACC_CAN_OK;
}

static bool bxcan_receive(void *vctx, acc_can_frame_t *frame)
{
    acc_can_bxcan_ctx_t *ctx = (acc_can_bxcan_ctx_t *)vctx;
    return anv_frame_ring_pop(&ctx->rx_ring, frame);
}

/* ABOM means a transition back to error-active after bus-off happens in hardware; this just
 * keeps the cached state (and the bus_off_count that DRIVE_TELEM_0 reports) in step with it. */
static void refresh_error_state(acc_can_bxcan_ctx_t *ctx)
{
    uint32_t esr = ctx->hcan->Instance->ESR;
    bool was_bus_off = (ctx->error_state.state == ACC_CAN_BUS_OFF);
    bool bus_off = (esr & CAN_ESR_BOFF) != 0u;
    bool error_passive = (esr & CAN_ESR_EPVF) != 0u;

    ctx->error_state.tec = (uint8_t)((esr & CAN_ESR_TEC) >> CAN_ESR_TEC_Pos);
    ctx->error_state.rec = (uint8_t)((esr & CAN_ESR_REC) >> CAN_ESR_REC_Pos);

    if (bus_off) {
        ctx->error_state.state = ACC_CAN_BUS_OFF;
        if (!was_bus_off) {
            ctx->error_state.bus_off_count++;
        }
    } else if (error_passive) {
        ctx->error_state.state = ACC_CAN_ERROR_PASSIVE;
    } else {
        ctx->error_state.state = ACC_CAN_ERROR_ACTIVE;
    }
}

static int bxcan_get_error_state(void *vctx, acc_can_error_state_t *state)
{
    acc_can_bxcan_ctx_t *ctx = (acc_can_bxcan_ctx_t *)vctx;
    refresh_error_state(ctx);
    *state = ctx->error_state;
    return ACC_CAN_OK;
}

/* bxCAN recovers from bus-off by itself (ABOM, enabled in bxcan_init()); there is nothing for
 * software to do, but the core calls this unconditionally on every backend so it must exist. */
static int bxcan_start_recovery(void *vctx)
{
    (void)vctx;
    return ACC_CAN_OK;
}

acc_can_driver_t acc_can_bxcan_make_driver(acc_can_bxcan_ctx_t *ctx, CAN_HandleTypeDef *hcan)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->hcan = hcan;

    acc_can_driver_t driver;
    driver.ctx = ctx;
    driver.init = bxcan_init;
    driver.send = bxcan_send;
    driver.receive = bxcan_receive;
    driver.get_error_state = bxcan_get_error_state;
    driver.start_recovery = bxcan_start_recovery;
    return driver;
}

void acc_can_bxcan_isr_rx_fifo0(acc_can_bxcan_ctx_t *ctx)
{
    CAN_RxHeaderTypeDef header;
    acc_can_frame_t frame;

    if (HAL_CAN_GetRxMessage(ctx->hcan, CAN_RX_FIFO0, &header, frame.data) != HAL_OK) {
        return;
    }
    frame.id = (uint16_t)header.StdId;
    frame.dlc = (uint8_t)header.DLC;
    (void)anv_frame_ring_push(&ctx->rx_ring, &frame);
}

void acc_can_bxcan_isr_error(acc_can_bxcan_ctx_t *ctx)
{
    refresh_error_state(ctx);
}
