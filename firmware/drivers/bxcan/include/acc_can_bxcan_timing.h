/* 500 kbps bit timing rows for STM32 bxCAN, from docs/architecture.md ("Controller setup per
 * device" -> STM32 nodes table). Keyed on ACC_CAN_BXCAN_CLOCK_HZ (the CAN peripheral clock,
 * APB1 for bxCAN), which the board defines before including acc_can_bxcan.h. Picking the row
 * at compile time (rather than computing it at runtime) means a wrong board clock is caught
 * by the build, not by a bus full of error frames.
 *
 * Each row's prescaler and TQ counts are checked against the 500 kbps target below, so a typo
 * here fails the build instead of silently bringing a node up at the wrong bitrate.
 */
#ifndef ACC_CAN_BXCAN_TIMING_H
#define ACC_CAN_BXCAN_TIMING_H

#ifndef ACC_CAN_BXCAN_CLOCK_HZ
#error "Define ACC_CAN_BXCAN_CLOCK_HZ (the bxCAN peripheral clock, e.g. APB1) before including acc_can_bxcan.h"
#endif

/* No 'u' suffix on these: they are token-pasted into CAN_BSn_<N>TQ macro names below, and
 * "11u" ## "TQ" would paste into the nonexistent token "11uTQ". */
#if ACC_CAN_BXCAN_CLOCK_HZ == 36000000
/* F103: 36 MHz APB1 */
#define ACC_CAN_BXCAN_PRESCALER 4
#define ACC_CAN_BXCAN_BS1_TQ 13
#define ACC_CAN_BXCAN_BS2_TQ 4
#elif ACC_CAN_BXCAN_CLOCK_HZ == 42000000
/* F407 / F405: 42 MHz APB1 */
#define ACC_CAN_BXCAN_PRESCALER 4
#define ACC_CAN_BXCAN_BS1_TQ 16
#define ACC_CAN_BXCAN_BS2_TQ 4
#elif ACC_CAN_BXCAN_CLOCK_HZ == 45000000
/* F446: 45 MHz APB1 */
#define ACC_CAN_BXCAN_PRESCALER 6
#define ACC_CAN_BXCAN_BS1_TQ 11
#define ACC_CAN_BXCAN_BS2_TQ 3
#else
#error "No bxCAN 500 kbps timing row for this ACC_CAN_BXCAN_CLOCK_HZ; add one to acc_can_bxcan_timing.h (docs/architecture.md, Controller setup per device)"
#endif

#define ACC_CAN_BXCAN_TQ_PER_BIT (1u + ACC_CAN_BXCAN_BS1_TQ + ACC_CAN_BXCAN_BS2_TQ)

/* Pulled in only for the static_assert below; harmless to include twice. */
#include <assert.h>

static_assert((ACC_CAN_BXCAN_CLOCK_HZ / ACC_CAN_BXCAN_PRESCALER) / ACC_CAN_BXCAN_TQ_PER_BIT == 500000u,
              "bxCAN timing row does not produce 500 kbps for this ACC_CAN_BXCAN_CLOCK_HZ");
static_assert((ACC_CAN_BXCAN_CLOCK_HZ / ACC_CAN_BXCAN_PRESCALER) % ACC_CAN_BXCAN_TQ_PER_BIT == 0u,
              "bxCAN timing row leaves a remainder: clock / prescaler must divide evenly by the TQ count");

/* Sample point must land in 75%-81% (docs/architecture.md). (1 + BS1) / TQ_PER_BIT, in
 * permille to stay in integer arithmetic. */
#define ACC_CAN_BXCAN_SAMPLE_POINT_PERMILLE (((1u + ACC_CAN_BXCAN_BS1_TQ) * 1000u) / ACC_CAN_BXCAN_TQ_PER_BIT)
static_assert(ACC_CAN_BXCAN_SAMPLE_POINT_PERMILLE >= 750u && ACC_CAN_BXCAN_SAMPLE_POINT_PERMILLE <= 810u,
              "bxCAN sample point out of the 75%-81% range required by docs/architecture.md");

/* HAL_CAN wants the BS1/BS2 fields as the CAN_BS1_nTQ / CAN_BS2_nTQ macros, not raw counts. */
#define ACC_CAN_BXCAN_CONCAT_(a, b) a##b
#define ACC_CAN_BXCAN_CONCAT(a, b) ACC_CAN_BXCAN_CONCAT_(a, b)
#define ACC_CAN_BXCAN_HAL_BS1 ACC_CAN_BXCAN_CONCAT(CAN_BS1_, ACC_CAN_BXCAN_CONCAT(ACC_CAN_BXCAN_BS1_TQ, TQ))
#define ACC_CAN_BXCAN_HAL_BS2 ACC_CAN_BXCAN_CONCAT(CAN_BS2_, ACC_CAN_BXCAN_CONCAT(ACC_CAN_BXCAN_BS2_TQ, TQ))

#endif /* ACC_CAN_BXCAN_TIMING_H */
