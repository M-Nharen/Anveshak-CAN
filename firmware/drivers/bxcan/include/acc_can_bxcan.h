/* bxCAN driver (STM32 F0/F1/F4 CAN peripheral, via the ST HAL) implementing acc_can_driver_t
 * (acc_can.h) so anveshak_can_core never depends on a specific peripheral.
 *
 * docs/architecture.md, "Controller setup per device" and "Hardware filters on nodes":
 *   - 500 kbps timing picked at compile time (see acc_can_bxcan_timing.h), keyed on
 *     ACC_CAN_BXCAN_CLOCK_HZ.
 *   - ABOM on (automatic bus-off recovery), TXFP on (mailboxes transmit in request order),
 *     NART off (automatic retransmission stays on).
 *   - Two 32-bit mask filters: node-addressed (mask 0x0F8) and class 0 (mask 0x700).
 *   - RX is interrupt-driven into the core's ring buffer; the application never touches the
 *     hardware FIFO directly.
 *
 * Which family's HAL header to pull in is selected by the board (one of
 * ACC_CAN_BXCAN_FAMILY_F4 / _F1 / _F0), so this one driver source serves all three bxCAN
 * families without a different build per family.
 */
#ifndef ACC_CAN_BXCAN_H
#define ACC_CAN_BXCAN_H

#if defined(ACC_CAN_BXCAN_FAMILY_F4)
#include "stm32f4xx_hal.h"
#elif defined(ACC_CAN_BXCAN_FAMILY_F1)
#include "stm32f1xx_hal.h"
#elif defined(ACC_CAN_BXCAN_FAMILY_F0)
#include "stm32f0xx_hal.h"
#else
#error "Define ACC_CAN_BXCAN_FAMILY_F4, ACC_CAN_BXCAN_FAMILY_F1 or ACC_CAN_BXCAN_FAMILY_F0 to pick the HAL family this bxCAN build targets"
#endif

#include "acc_can.h"
#include "anv_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ACC_CAN_BXCAN_RX_CAPACITY 32u

typedef struct {
    CAN_HandleTypeDef *hcan;
    anv_frame_ring_t rx_ring;
    acc_can_frame_t rx_storage[ACC_CAN_BXCAN_RX_CAPACITY];
    acc_can_error_state_t error_state;
} acc_can_bxcan_ctx_t;

/* Builds the acc_can_driver_t vtable backed by `ctx` and `hcan`. `hcan->Instance` must already
 * name the CAN peripheral to use (e.g. CAN1); clocks, GPIO alternate function and NVIC are the
 * board's job (HAL_CAN_MspInit), not this driver's. driver.init() programs bit timing, mode
 * flags and the two filters from docs/architecture.md and calls HAL_CAN_Start() plus
 * HAL_CAN_ActivateNotification() for RX FIFO 0 pending, bus-off and error interrupts. */
acc_can_driver_t acc_can_bxcan_make_driver(acc_can_bxcan_ctx_t *ctx, CAN_HandleTypeDef *hcan);

/* Call from this instance's HAL_CAN_RxFifo0MsgPendingCallback(): reads the pending frame out
 * of hardware FIFO 0 and pushes it onto the ring buffer. Interrupt-safe; does no other work
 * (docs/architecture.md, "CAN handling inside the node" -- never do real work in the ISR). */
void acc_can_bxcan_isr_rx_fifo0(acc_can_bxcan_ctx_t *ctx);

/* Call from this instance's HAL_CAN_ErrorCallback(): refreshes the cached error/bus-off state
 * from the hardware so the next get_error_state() call sees it. ABOM means the peripheral
 * itself rejoins the bus after bus-off; this driver's start_recovery() is a no-op for bxCAN
 * and exists only so the core's generic recovery path has something to call. */
void acc_can_bxcan_isr_error(acc_can_bxcan_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ACC_CAN_BXCAN_H */
