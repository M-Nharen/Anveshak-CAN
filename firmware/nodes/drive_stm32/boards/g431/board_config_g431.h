/* Pin, timer and clock assignments for the STM32G431 (FDCAN) drive board variant. This is the
 * one file to edit when wiring this firmware to a real PCB -- nothing else in app/ or src/
 * names a pin or timer directly. Placeholder values below follow a Nucleo-G431RB-style
 * layout; replace them with the real board's assignment.
 *
 * Must be included after stm32g4xx_hal.h (via board_config.h, which this build always reaches
 * through acc_can_fdcan.h first), since GPIOx, TIMx, ADC1, GPIO_PIN_n and GPIO_AF9_FDCAN1 all
 * come from the CMSIS device header pulled in by the HAL.
 */
#ifndef DRIVE_BOARD_CONFIG_G431_H
#define DRIVE_BOARD_CONFIG_G431_H

/* TCAN1044AV transceiver (docs/architecture.md, "Physical layer"): STB driven low at start-up
 * so the node is not stuck in standby (CLAUDE.md / prompts/A2, "TCAN1044AV handling"). */
#define DRIVE_CAN_GPIO_PORT GPIOA
#define DRIVE_CAN_TX_PIN GPIO_PIN_12
#define DRIVE_CAN_RX_PIN GPIO_PIN_11
#define DRIVE_CAN_GPIO_AF GPIO_AF9_FDCAN1
#define DRIVE_TCAN_STB_PORT GPIOB
#define DRIVE_TCAN_STB_PIN GPIO_PIN_12

/* 5-bit node ID switch (docs/architecture.md, "Node ID and status LEDs"): all five lines on
 * one port so reading it is a single masked register read. */
#define DRIVE_NODE_ID_PORT GPIOC
#define DRIVE_NODE_ID_PIN0 GPIO_PIN_0
#define DRIVE_NODE_ID_PIN1 GPIO_PIN_1
#define DRIVE_NODE_ID_PIN2 GPIO_PIN_2
#define DRIVE_NODE_ID_PIN3 GPIO_PIN_3
#define DRIVE_NODE_ID_PIN4 GPIO_PIN_4

/* Motor channel A: PWM + direction + quadrature encoder. */
#define DRIVE_MOTOR_A_PWM_TIM TIM1
#define DRIVE_MOTOR_A_PWM_CHANNEL TIM_CHANNEL_1
#define DRIVE_MOTOR_A_PWM_GPIO_PORT GPIOA
#define DRIVE_MOTOR_A_PWM_PIN GPIO_PIN_8
#define DRIVE_MOTOR_A_PWM_GPIO_AF GPIO_AF2_TIM1
#define DRIVE_MOTOR_A_DIR_PORT GPIOA
#define DRIVE_MOTOR_A_DIR_PIN GPIO_PIN_4
#define DRIVE_MOTOR_A_ENCODER_TIM TIM2
#define DRIVE_MOTOR_A_ENCODER_GPIO_PORT GPIOA
#define DRIVE_MOTOR_A_ENCODER_PIN_CH1 GPIO_PIN_0
#define DRIVE_MOTOR_A_ENCODER_PIN_CH2 GPIO_PIN_1
#define DRIVE_MOTOR_A_ENCODER_GPIO_AF GPIO_AF1_TIM2

/* Motor channel B: PWM + direction + quadrature encoder. */
#define DRIVE_MOTOR_B_PWM_TIM TIM1
#define DRIVE_MOTOR_B_PWM_CHANNEL TIM_CHANNEL_2
#define DRIVE_MOTOR_B_PWM_GPIO_PORT GPIOA
#define DRIVE_MOTOR_B_PWM_PIN GPIO_PIN_9
#define DRIVE_MOTOR_B_PWM_GPIO_AF GPIO_AF2_TIM1
#define DRIVE_MOTOR_B_DIR_PORT GPIOA
#define DRIVE_MOTOR_B_DIR_PIN GPIO_PIN_5
#define DRIVE_MOTOR_B_ENCODER_TIM TIM3
#define DRIVE_MOTOR_B_ENCODER_GPIO_PORT GPIOB
#define DRIVE_MOTOR_B_ENCODER_PIN_CH1 GPIO_PIN_4
#define DRIVE_MOTOR_B_ENCODER_PIN_CH2 GPIO_PIN_5
#define DRIVE_MOTOR_B_ENCODER_GPIO_AF GPIO_AF2_TIM3

/* 1 kHz PID loop timer (docs/architecture.md, "The motor PID runs locally ... 1 kHz on a
 * timer interrupt"). A basic timer is enough: it only needs to tick, not to drive a pin. */
#define DRIVE_PID_TIM TIM7
#define DRIVE_PID_TIM_IRQ TIM7_IRQn
#define DRIVE_PID_RATE_HZ 1000u

/* Current sensing (one ADC, one channel per motor channel). */
#define DRIVE_CURRENT_ADC ADC1
#define DRIVE_CURRENT_A_ADC_CHANNEL ADC_CHANNEL_1
#define DRIVE_CURRENT_B_ADC_CHANNEL ADC_CHANNEL_2

/* Status LED (docs/architecture.md, "Node ID and status LEDs"). */
#define DRIVE_STATUS_LED_PORT GPIOA
#define DRIVE_STATUS_LED_PIN GPIO_PIN_6

/* Hardware watchdog (docs/architecture.md, "Watchdogs: three layers", layer 2). IWDG's LSI
 * clock is approximate, so this is deliberately short of the ~50 ms budget. */
#define DRIVE_IWDG_TIMEOUT_MS 40u

/* Black-box log (anv_diag.h): the last 16 KB (8 pages of 2 KB) of the 128 KB flash, used as a
 * ring of 512-byte slots so it is erased only once per 32 saves (board_log_flash.h). Keep this
 * clear of the bootloader and application image when the linker script is written. */
#define DRIVE_LOG_FLASH_BASE 0x0801C000u
#define DRIVE_LOG_FLASH_SIZE (16u * 1024u)

#endif /* DRIVE_BOARD_CONFIG_G431_H */
