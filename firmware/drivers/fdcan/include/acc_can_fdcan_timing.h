/* 500 kbps bit timing rows for STM32 FDCAN run in classic mode, from docs/architecture.md
 * ("Controller setup per device" -> STM32 nodes table). Keyed on ACC_CAN_FDCAN_CLOCK_HZ (the
 * FDCAN kernel clock the board's clock tree feeds it), defined before including
 * acc_can_fdcan.h. As with bxCAN, the row is picked and checked at compile time so a wrong
 * board clock fails the build rather than filling the bus with error frames.
 */
#ifndef ACC_CAN_FDCAN_TIMING_H
#define ACC_CAN_FDCAN_TIMING_H

#ifndef ACC_CAN_FDCAN_CLOCK_HZ
#error "Define ACC_CAN_FDCAN_CLOCK_HZ (the FDCAN kernel clock) before including acc_can_fdcan.h"
#endif

#if ACC_CAN_FDCAN_CLOCK_HZ == 160000000
/* G431 / G474, classic mode: 160 MHz kernel clock */
#define ACC_CAN_FDCAN_PRESCALER 16u
#define ACC_CAN_FDCAN_BS1_TQ 15u
#define ACC_CAN_FDCAN_BS2_TQ 4u
#elif ACC_CAN_FDCAN_CLOCK_HZ == 64000000
/* G0B1, classic mode: 64 MHz kernel clock */
#define ACC_CAN_FDCAN_PRESCALER 8u
#define ACC_CAN_FDCAN_BS1_TQ 12u
#define ACC_CAN_FDCAN_BS2_TQ 3u
#else
#error "No FDCAN 500 kbps timing row for this ACC_CAN_FDCAN_CLOCK_HZ; add one to acc_can_fdcan_timing.h (docs/architecture.md, Controller setup per device)"
#endif

#define ACC_CAN_FDCAN_TQ_PER_BIT (1u + ACC_CAN_FDCAN_BS1_TQ + ACC_CAN_FDCAN_BS2_TQ)

#include <assert.h>

static_assert((ACC_CAN_FDCAN_CLOCK_HZ / ACC_CAN_FDCAN_PRESCALER) / ACC_CAN_FDCAN_TQ_PER_BIT == 500000u,
              "FDCAN timing row does not produce 500 kbps for this ACC_CAN_FDCAN_CLOCK_HZ");
static_assert((ACC_CAN_FDCAN_CLOCK_HZ / ACC_CAN_FDCAN_PRESCALER) % ACC_CAN_FDCAN_TQ_PER_BIT == 0u,
              "FDCAN timing row leaves a remainder: clock / prescaler must divide evenly by the TQ count");

#define ACC_CAN_FDCAN_SAMPLE_POINT_PERMILLE (((1u + ACC_CAN_FDCAN_BS1_TQ) * 1000u) / ACC_CAN_FDCAN_TQ_PER_BIT)
static_assert(ACC_CAN_FDCAN_SAMPLE_POINT_PERMILLE >= 750u && ACC_CAN_FDCAN_SAMPLE_POINT_PERMILLE <= 810u,
              "FDCAN sample point out of the 75%-81% range required by docs/architecture.md");

#endif /* ACC_CAN_FDCAN_TIMING_H */
