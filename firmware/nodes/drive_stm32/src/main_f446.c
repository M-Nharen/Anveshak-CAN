/* Drive node application entry point, STM32F446 (bxCAN) variant. Wires board_hw_f446.c's
 * hardware hooks and acc_can_bxcan into drive_app_t, then runs the main loop:
 * receive -> dispatch, tick for watchdogs/heartbeat/telemetry. The 1 kHz PID step runs from
 * TIM7's interrupt (board_hw_f446.c) via board_hw_pid_tick() below.
 *
 * image.cmake links this into a flashable image that runs behind the CAN bootloader
 * (ld/drive_app_f446.ld, ST's startup file, src/app_image.c). SystemClock_Config() below is
 * still a placeholder (see README.md).
 */
#include "board_hw_f446.h"

#include "acc_can_bxcan.h"
#include "app_image.h"
#include "board_config.h"
#include "drive_app.h"
#include "ids.h"

static acc_can_bxcan_ctx_t s_can_ctx;
static drive_app_t s_app;

static anv_param_descriptor_t s_param_descriptors[ROVER_PARAMS_DRIVE_TABLE_LEN];
static anv_param_value_t s_param_values[ROVER_PARAMS_DRIVE_TABLE_LEN];
static anv_param_store_t s_param_store;

#define DRIVE_TXQ_CAP_PER_PRIO 8u
static acc_can_frame_t s_txq_storage[ACC_CAN_PRIO_COUNT * DRIVE_TXQ_CAP_PER_PRIO];

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    (void)hcan;
    acc_can_bxcan_isr_rx_fifo0(&s_can_ctx);
}

void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *hcan)
{
    (void)hcan;
    acc_can_bxcan_isr_error(&s_can_ctx);
}

void board_hw_pid_tick(void)
{
    drive_app_motor_step(&s_app, 1000u / DRIVE_PID_RATE_HZ);
}

/* Placeholder: the real PLL configuration depends on the final board's crystal and is a
 * CubeMX job once the exact part is chosen (CLAUDE.md / prompts/A2: "We don't know the exact
 * STM32 part yet"). It must land APB1 (the bxCAN clock) on ACC_CAN_BXCAN_CLOCK_HZ
 * (45 MHz for F446, board_config_f446.h / acc_can_bxcan_timing.h). */
static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState = RCC_HSE_ON;
    osc.PLL.PLLState = RCC_PLL_NONE;
    (void)HAL_RCC_OscConfig(&osc);

    RCC_ClkInitTypeDef clk = {0};
    clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_HSE;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    (void)HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_0);
}

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    board_hw_init();

    uint8_t node_id = board_hw_board_hooks()->read_node_id(NULL);

    drive_app_build_param_descriptors(s_param_descriptors);
    anv_param_store_init(&s_param_store, s_param_values, s_param_descriptors, ROVER_PARAMS_DRIVE_TABLE_LEN,
                          board_hw_board_hooks());
    anv_param_store_load(&s_param_store);

    hcan1.Instance = CAN1;
    acc_can_driver_t can_driver = acc_can_bxcan_make_driver(&s_can_ctx, &hcan1);

    /* Two hardware mask filters (docs/architecture.md, "Hardware filters on nodes"): anything
     * addressed to this node (mask 0x0F8, the node field), plus all of class 0 (mask 0x700). */
    acc_can_filter_t filters[2];
    filters[0].id = (uint16_t)(node_id << 3);
    filters[0].mask = 0x0F8u;
    filters[1].id = 0x000u;
    filters[1].mask = 0x700u;
    (void)can_driver.init(can_driver.ctx, 500000u, filters, 2u);

    anv_node_info_t info = {0};
    info.git_build_id = app_image_build_id();
    uint32_t now_ms = HAL_GetTick();
    drive_app_init(&s_app, node_id, &can_driver, s_txq_storage, DRIVE_TXQ_CAP_PER_PRIO, &s_param_store, &info,
                   board_hw_board_hooks(), board_hw_motor_hw(), board_hw_telem_hw(), now_ms);

    for (;;) {
        acc_can_frame_t frame;
        while (can_driver.receive(can_driver.ctx, &frame)) {
            drive_app_on_rx_frame(&s_app, &frame, HAL_GetTick());
        }
        drive_app_tick(&s_app, HAL_GetTick());
        (void)anv_node_drain_tx(&s_app.node);
    }
}
