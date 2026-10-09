/* FDCAN driver (STM32 G0/G4/H7 FDCAN peripheral, run in classic-CAN-only mode via the ST HAL)
 * implementing acc_can_driver_t (acc_can.h).
 *
 * docs/architecture.md, "Controller setup per device" and "Hardware filters on nodes":
 *   - Classic frame format, bit-rate switching off; automatic retransmission stays on.
 *   - 500 kbps timing picked at compile time (acc_can_fdcan_timing.h), keyed on
 *     ACC_CAN_FDCAN_CLOCK_HZ.
 *   - Two standard-ID mask filter elements: node-addressed (mask 0x0F8) and class 0
 *     (mask 0x700), both routed to Rx FIFO 0.
 *   - FDCAN does not leave bus-off by itself: start_recovery() calls HAL_FDCAN_Start() again,
 *     which clears the CCCR.INIT bit hardware sets on bus-off.
 *
 * Which family's HAL header to pull in is selected by the board (one of
 * ACC_CAN_FDCAN_FAMILY_G4 / _G0 / _H7).
 */
#ifndef ACC_CAN_FDCAN_H
#define ACC_CAN_FDCAN_H

#if defined(ACC_CAN_FDCAN_FAMILY_G4)
#include "stm32g4xx_hal.h"
#elif defined(ACC_CAN_FDCAN_FAMILY_G0)
#include "stm32g0xx_hal.h"
#elif defined(ACC_CAN_FDCAN_FAMILY_H7)
#include "stm32h7xx_hal.h"
#else
#error "Define ACC_CAN_FDCAN_FAMILY_G4, ACC_CAN_FDCAN_FAMILY_G0 or ACC_CAN_FDCAN_FAMILY_H7 to pick the HAL family this FDCAN build targets"
#endif

#include "acc_can.h"
#include "anv_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ACC_CAN_FDCAN_RX_CAPACITY 32u

typedef struct {
    FDCAN_HandleTypeDef *hfdcan;
    anv_frame_ring_t rx_ring;
    acc_can_frame_t rx_storage[ACC_CAN_FDCAN_RX_CAPACITY];
    acc_can_error_state_t error_state;
} acc_can_fdcan_ctx_t;

/* Builds the acc_can_driver_t vtable backed by `ctx` and `hfdcan`. `hfdcan->Instance` must
 * already name the peripheral (e.g. FDCAN1); clocks, GPIO alternate function and NVIC are the
 * board's job (HAL_FDCAN_MspInit), not this driver's. */
acc_can_driver_t acc_can_fdcan_make_driver(acc_can_fdcan_ctx_t *ctx, FDCAN_HandleTypeDef *hfdcan);

/* Call from this instance's HAL_FDCAN_RxFifo0Callback() when RxFifo0ITs has
 * FDCAN_IT_RX_FIFO0_NEW_MESSAGE set: drains the new message(s) out of hardware Rx FIFO 0 into
 * the ring buffer. Interrupt-safe; does no other work. */
void acc_can_fdcan_isr_rx_fifo0(acc_can_fdcan_ctx_t *ctx);

/* Call from this instance's HAL_FDCAN_ErrorStatusCallback(): refreshes the cached error
 * counters and bus-off state from the hardware (HAL_FDCAN_GetProtocolStatus /
 * HAL_FDCAN_GetErrorCounters). */
void acc_can_fdcan_isr_error(acc_can_fdcan_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ACC_CAN_FDCAN_H */
