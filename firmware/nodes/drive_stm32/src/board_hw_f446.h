/* Board bring-up for the STM32F446 (bxCAN) variant: GPIO, timers, ADC and IWDG, plus the
 * anv_board_hooks_t / drive_motor_hw_t / drive_telem_hw_t vtables main_f446.c wires into
 * drive_app. Pin and timer assignments all come from board_config.h -- nothing here is board-
 * specific beyond what that file says. */
#ifndef BOARD_HW_F446_H
#define BOARD_HW_F446_H

#include "stm32f4xx_hal.h"

#include "anv_board.h"
#include "drive_app.h"

#ifdef __cplusplus
extern "C" {
#endif

extern CAN_HandleTypeDef hcan1;

/* Brings up every peripheral this node uses: GPIO (CAN, TCAN STB, node ID switch, status LED,
 * motor direction), TIM1 (PWM), TIM2/TIM3 (quadrature encoders), TIM7 (1 kHz PID tick), ADC1
 * (current sensing) and IWDG. Does not touch the CAN peripheral itself -- that is
 * acc_can_bxcan's job once this returns. */
void board_hw_init(void);

const anv_board_hooks_t *board_hw_board_hooks(void);
const drive_motor_hw_t *board_hw_motor_hw(void);
const drive_telem_hw_t *board_hw_telem_hw(void);

/* Implemented by main_f446.c; called from TIM7's interrupt at DRIVE_PID_RATE_HZ. */
void board_hw_pid_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_HW_F446_H */
