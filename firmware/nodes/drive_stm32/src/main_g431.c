/* Drive node application entry point, STM32G431 (FDCAN) variant. Wires board_hw_g431.c's
 * hardware hooks and acc_can_fdcan into drive_app_t, then runs the main loop:
 * receive -> dispatch, tick for watchdogs/heartbeat/telemetry. The 1 kHz PID step runs from
 * TIM7's interrupt (board_hw_g431.c) via board_hw_pid_tick() below.
 *
 * This builds as a static library, not a linked firmware image (see CMakeLists.txt): there is
 * no startup code, vector table or linker script here, so main()/SystemClock_Config() below
 * prove the wiring compiles but are never actually executed. A real board project supplies
 * those and links this code into it.
 */
#include "board_hw_g431.h"

#include "acc_can_fdcan.h"
#include "board_config.h"
#include "drive_app.h"
#include "ids.h"

static acc_can_fdcan_ctx_t s_can_ctx;
static drive_app_t s_app;

static anv_param_descriptor_t s_param_descriptors[ROVER_PARAMS_DRIVE_TABLE_LEN];
static anv_param_value_t s_param_values[ROVER_PARAMS_DRIVE_TABLE_LEN];
static anv_param_store_t s_param_store;

#define DRIVE_TXQ_CAP_PER_PRIO 8u
static acc_can_frame_t s_txq_storage[ACC_CAN_PRIO_COUNT * DRIVE_TXQ_CAP_PER_PRIO];

void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs)
{
    (void)hfdcan;
    if ((RxFifo0ITs & FDCAN_IT_RX_FIFO0_NEW_MESSAGE) != 0u) {
        acc_can_fdcan_isr_rx_fifo0(&s_can_ctx);
    }
}

void HAL_FDCAN_ErrorStatusCallback(FDCAN_HandleTypeDef *hfdcan, uint32_t ErrorStatusITs)
{
    (void)hfdcan;
    (void)ErrorStatusITs;
    acc_can_fdcan_isr_error(&s_can_ctx);
}

void board_hw_pid_tick(void)
{
    drive_app_motor_step(&s_app, 1000u / DRIVE_PID_RATE_HZ);
}

/* Placeholder: the real PLL configuration depends on the final board's crystal and is a
 * CubeMX job once the exact part is chosen (CLAUDE.md / prompts/A2: "We don't know the exact
 * STM32 part yet"). It must land the FDCAN kernel clock on ACC_CAN_FDCAN_CLOCK_HZ
 * (160 MHz for G431, board_config_g431.h / acc_can_fdcan_timing.h). */
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

    hfdcan1.Instance = FDCAN1;
    acc_can_driver_t can_driver = acc_can_fdcan_make_driver(&s_can_ctx, &hfdcan1);

    /* Two hardware mask filters (docs/architecture.md, "Hardware filters on nodes"): anything
     * addressed to this node (mask 0x0F8, the node field), plus all of class 0 (mask 0x700). */
    acc_can_filter_t filters[2];
    filters[0].id = (uint16_t)(node_id << 3);
    filters[0].mask = 0x0F8u;
    filters[1].id = 0x000u;
    filters[1].mask = 0x700u;
    (void)can_driver.init(can_driver.ctx, 500000u, filters, 2u);

    anv_node_info_t info = {0};
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
