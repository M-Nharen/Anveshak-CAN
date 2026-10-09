#include "board_hw_f446.h"

#include <string.h>

#include "board_config.h"
#include "board_crash.h"
#include "board_log_flash.h"
#include "ids.h"

CAN_HandleTypeDef hcan1;

static TIM_HandleTypeDef htim_pwm;
static TIM_HandleTypeDef htim_enc_a;
static TIM_HandleTypeDef htim_enc_b;
static TIM_HandleTypeDef htim_pid;
static ADC_HandleTypeDef hadc1;
static IWDG_HandleTypeDef hiwdg;

#define DRIVE_PWM_PERIOD 999u /* CCR is scaled 0..DRIVE_PWM_PERIOD for 0..100% duty */

/* ---- anv_board_hooks_t -------------------------------------------------------------- */

static uint8_t board_read_node_id(void *ctx)
{
    (void)ctx;
    uint8_t id = 0;
    if (HAL_GPIO_ReadPin(DRIVE_NODE_ID_PORT, DRIVE_NODE_ID_PIN0) != GPIO_PIN_RESET) {
        id |= (1u << 0);
    }
    if (HAL_GPIO_ReadPin(DRIVE_NODE_ID_PORT, DRIVE_NODE_ID_PIN1) != GPIO_PIN_RESET) {
        id |= (1u << 1);
    }
    if (HAL_GPIO_ReadPin(DRIVE_NODE_ID_PORT, DRIVE_NODE_ID_PIN2) != GPIO_PIN_RESET) {
        id |= (1u << 2);
    }
    if (HAL_GPIO_ReadPin(DRIVE_NODE_ID_PORT, DRIVE_NODE_ID_PIN3) != GPIO_PIN_RESET) {
        id |= (1u << 3);
    }
    if (HAL_GPIO_ReadPin(DRIVE_NODE_ID_PORT, DRIVE_NODE_ID_PIN4) != GPIO_PIN_RESET) {
        id |= (1u << 4);
    }
    return id;
}

static void board_set_outputs_off(void *ctx)
{
    (void)ctx;
    __HAL_TIM_SET_COMPARE(&htim_pwm, DRIVE_MOTOR_A_PWM_CHANNEL, 0u);
    __HAL_TIM_SET_COMPARE(&htim_pwm, DRIVE_MOTOR_B_PWM_CHANNEL, 0u);
}

static void board_feed_hw_watchdog(void *ctx)
{
    (void)ctx;
    (void)HAL_IWDG_Refresh(&hiwdg);
}

static uint32_t board_now_ms(void *ctx)
{
    (void)ctx;
    return HAL_GetTick();
}


/* ---- diagnostics hooks: black-box flash log, crash record, reset cause, self-test ----------- */

static int log_erase(void *ctx)
{
    (void)ctx;
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t sector_error = 0u;
    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    erase.Sector = DRIVE_LOG_FLASH_SECTOR;
    erase.NbSectors = 1u;
    (void)HAL_FLASH_Unlock();
    HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&erase, &sector_error);
    (void)HAL_FLASH_Lock();
    return st == HAL_OK ? 0 : -1;
}

static int log_program(void *ctx, uint32_t offset, const uint8_t *data, size_t len)
{
    (void)ctx;
    int rc = 0;
    (void)HAL_FLASH_Unlock();
    for (size_t i = 0; i < len && rc == 0; i += 4u) {
        uint32_t word;
        memcpy(&word, data + i, sizeof(word));
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, DRIVE_LOG_FLASH_BASE + offset + (uint32_t)i, word) != HAL_OK) {
            rc = -1;
        }
    }
    (void)HAL_FLASH_Lock();
    return rc;
}

static board_log_flash_t s_log_flash = {
    .mem = (const uint8_t *)DRIVE_LOG_FLASH_BASE,
    .size = DRIVE_LOG_FLASH_SIZE,
    .ctx = NULL,
    .erase = log_erase,
    .program = log_program,
};

static int board_log_load(void *ctx, uint8_t *data, size_t len)
{
    (void)ctx;
    return board_log_flash_load(&s_log_flash, data, len);
}

static int board_log_save(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    return board_log_flash_save(&s_log_flash, data, len);
}

/* Power-on self-test (bit n = test n+1, see anv_board.h). Only the node ID switch can be checked
 * on this template: the supply, driver-fault, encoder and sensor readings are stubs here (see
 * telem_supply_mv), so those tests report "passed" until a real board wires them. */
static uint32_t board_self_test(void *ctx)
{
    uint32_t failed = 0u;
    uint8_t id = board_read_node_id(ctx);
    if (id < ROVER_NODE_DRIVE1 || id > ROVER_NODE_DRIVE6) {
        failed |= (1u << 5);
    }
    return failed;
}

static anv_board_hooks_t s_board_hooks;

const anv_board_hooks_t *board_hw_board_hooks(void)
{
    return &s_board_hooks;
}

/* ---- drive_motor_hw_t ---------------------------------------------------------------- */

static void motor_set_duty(void *ctx, uint8_t ch, int16_t duty_cpercent)
{
    (void)ctx;
    bool negative = duty_cpercent < 0;
    uint32_t mag = (uint32_t)(negative ? -(int32_t)duty_cpercent : duty_cpercent); /* 0..10000 */
    uint32_t ccr = (mag * (DRIVE_PWM_PERIOD + 1u)) / 10000u;

    GPIO_TypeDef *dir_port = (ch == DRIVE_MOTOR_CHANNEL_A) ? DRIVE_MOTOR_A_DIR_PORT : DRIVE_MOTOR_B_DIR_PORT;
    uint16_t dir_pin = (ch == DRIVE_MOTOR_CHANNEL_A) ? DRIVE_MOTOR_A_DIR_PIN : DRIVE_MOTOR_B_DIR_PIN;
    uint32_t channel = (ch == DRIVE_MOTOR_CHANNEL_A) ? DRIVE_MOTOR_A_PWM_CHANNEL : DRIVE_MOTOR_B_PWM_CHANNEL;

    HAL_GPIO_WritePin(dir_port, dir_pin, negative ? GPIO_PIN_SET : GPIO_PIN_RESET);
    __HAL_TIM_SET_COMPARE(&htim_pwm, channel, ccr);
}

/* Coast vs. brake is an H-bridge driver wiring detail (a dedicated enable/brake pin on the
 * final board); this template only has a PWM + direction pair, so both just stop driving. */
static void motor_set_coast(void *ctx, uint8_t ch)
{
    motor_set_duty(ctx, ch, 0);
}

static void motor_set_brake(void *ctx, uint8_t ch)
{
    motor_set_duty(ctx, ch, 0);
}

static int32_t motor_read_encoder(void *ctx, uint8_t ch)
{
    (void)ctx;
    TIM_HandleTypeDef *htim = (ch == DRIVE_MOTOR_CHANNEL_A) ? &htim_enc_a : &htim_enc_b;
    /* TIM2/TIM3's 16-bit counter wraps; sign-extending through int16_t gives a signed delta
     * across a wrap the same way a 32-bit free-running count would. */
    return (int32_t)(int16_t)__HAL_TIM_GET_COUNTER(htim);
}

static uint32_t motor_read_current_ma(void *ctx, uint8_t ch)
{
    (void)ctx;
    uint32_t channel = (ch == DRIVE_MOTOR_CHANNEL_A) ? DRIVE_CURRENT_A_ADC_CHANNEL : DRIVE_CURRENT_B_ADC_CHANNEL;

    ADC_ChannelConfTypeDef channel_cfg = {0};
    channel_cfg.Channel = channel;
    channel_cfg.Rank = 1u;
    channel_cfg.SamplingTime = ADC_SAMPLETIME_15CYCLES;
    (void)HAL_ADC_ConfigChannel(&hadc1, &channel_cfg);
    (void)HAL_ADC_Start(&hadc1);
    (void)HAL_ADC_PollForConversion(&hadc1, 10u);
    uint32_t raw = HAL_ADC_GetValue(&hadc1); /* 0..4095 at 12-bit resolution */

    /* Placeholder scale: the real mA-per-LSB gain depends on the current-sense amplifier and
     * shunt resistor on the final board. This exercises the path end to end without claiming
     * a real calibration. */
    return raw * 10u;
}

static drive_motor_hw_t s_motor_hw;

const drive_motor_hw_t *board_hw_motor_hw(void)
{
    return &s_motor_hw;
}

/* ---- drive_telem_hw_t ------------------------------------------------------------------
 * Not wired to real sensors in this template: a real board needs a supply-voltage divider and
 * driver/MCU temperature sensors on their own ADC channels. Left as placeholders -- see the
 * PR description. */

static uint16_t telem_supply_mv(void *ctx)
{
    (void)ctx;
    return 0u;
}

static int8_t telem_temp_stub(void *ctx)
{
    (void)ctx;
    return 0;
}

static drive_telem_hw_t s_telem_hw;

const drive_telem_hw_t *board_hw_telem_hw(void)
{
    return &s_telem_hw;
}

/* ---- Bring-up -------------------------------------------------------------------------- */

static void gpio_init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};

    /* CAN TX/RX alternate function. */
    gpio.Pin = DRIVE_CAN_TX_PIN | DRIVE_CAN_RX_PIN;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = DRIVE_CAN_GPIO_AF;
    HAL_GPIO_Init(DRIVE_CAN_GPIO_PORT, &gpio);

    /* TCAN1044AV STB: driven low at start-up so the transceiver leaves standby
     * (docs/architecture.md "Physical layer"; CLAUDE.md / prompts/A2 "TCAN1044AV handling"). */
    gpio.Pin = DRIVE_TCAN_STB_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_PULLDOWN;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(DRIVE_TCAN_STB_PORT, &gpio);
    HAL_GPIO_WritePin(DRIVE_TCAN_STB_PORT, DRIVE_TCAN_STB_PIN, GPIO_PIN_RESET);

    /* 5-bit node ID switch. */
    gpio.Pin = DRIVE_NODE_ID_PIN0 | DRIVE_NODE_ID_PIN1 | DRIVE_NODE_ID_PIN2 | DRIVE_NODE_ID_PIN3 | DRIVE_NODE_ID_PIN4;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(DRIVE_NODE_ID_PORT, &gpio);

    /* Status LED. */
    gpio.Pin = DRIVE_STATUS_LED_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(DRIVE_STATUS_LED_PORT, &gpio);

    /* Motor direction pins. */
    gpio.Pin = DRIVE_MOTOR_A_DIR_PIN;
    HAL_GPIO_Init(DRIVE_MOTOR_A_DIR_PORT, &gpio);
    gpio.Pin = DRIVE_MOTOR_B_DIR_PIN;
    HAL_GPIO_Init(DRIVE_MOTOR_B_DIR_PORT, &gpio);

    /* PWM timer outputs. */
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Pin = DRIVE_MOTOR_A_PWM_PIN;
    gpio.Alternate = DRIVE_MOTOR_A_PWM_GPIO_AF;
    HAL_GPIO_Init(DRIVE_MOTOR_A_PWM_GPIO_PORT, &gpio);
    gpio.Pin = DRIVE_MOTOR_B_PWM_PIN;
    gpio.Alternate = DRIVE_MOTOR_B_PWM_GPIO_AF;
    HAL_GPIO_Init(DRIVE_MOTOR_B_PWM_GPIO_PORT, &gpio);

    /* Quadrature encoder inputs. */
    gpio.Pin = DRIVE_MOTOR_A_ENCODER_PIN_CH1 | DRIVE_MOTOR_A_ENCODER_PIN_CH2;
    gpio.Alternate = DRIVE_MOTOR_A_ENCODER_GPIO_AF;
    HAL_GPIO_Init(DRIVE_MOTOR_A_ENCODER_GPIO_PORT, &gpio);
    gpio.Pin = DRIVE_MOTOR_B_ENCODER_PIN_CH1 | DRIVE_MOTOR_B_ENCODER_PIN_CH2;
    gpio.Alternate = DRIVE_MOTOR_B_ENCODER_GPIO_AF;
    HAL_GPIO_Init(DRIVE_MOTOR_B_ENCODER_GPIO_PORT, &gpio);
}

static void pwm_init(void)
{
    __HAL_RCC_TIM1_CLK_ENABLE();

    htim_pwm.Instance = DRIVE_MOTOR_A_PWM_TIM;
    htim_pwm.Init.Prescaler = 0u; /* adjust once the real clock tree is set */
    htim_pwm.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim_pwm.Init.Period = DRIVE_PWM_PERIOD;
    htim_pwm.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim_pwm.Init.RepetitionCounter = 0u;
    htim_pwm.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    (void)HAL_TIM_PWM_Init(&htim_pwm);

    TIM_OC_InitTypeDef oc = {0};
    oc.OCMode = TIM_OCMODE_PWM1;
    oc.Pulse = 0u;
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCNPolarity = TIM_OCNPOLARITY_HIGH;
    oc.OCFastMode = TIM_OCFAST_DISABLE;
    oc.OCIdleState = TIM_OCIDLESTATE_RESET;
    oc.OCNIdleState = TIM_OCNIDLESTATE_RESET;
    (void)HAL_TIM_PWM_ConfigChannel(&htim_pwm, &oc, DRIVE_MOTOR_A_PWM_CHANNEL);
    (void)HAL_TIM_PWM_ConfigChannel(&htim_pwm, &oc, DRIVE_MOTOR_B_PWM_CHANNEL);
    (void)HAL_TIM_PWM_Start(&htim_pwm, DRIVE_MOTOR_A_PWM_CHANNEL);
    (void)HAL_TIM_PWM_Start(&htim_pwm, DRIVE_MOTOR_B_PWM_CHANNEL);
}

static void encoder_init_one(TIM_HandleTypeDef *htim, TIM_TypeDef *instance)
{
    htim->Instance = instance;
    htim->Init.Prescaler = 0u;
    htim->Init.CounterMode = TIM_COUNTERMODE_UP;
    htim->Init.Period = 0xFFFFu;
    htim->Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim->Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

    TIM_Encoder_InitTypeDef enc = {0};
    enc.EncoderMode = TIM_ENCODERMODE_TI12;
    enc.IC1Polarity = TIM_ICPOLARITY_RISING;
    enc.IC1Selection = TIM_ICSELECTION_DIRECTTI;
    enc.IC1Prescaler = TIM_ICPSC_DIV1;
    enc.IC1Filter = 0u;
    enc.IC2Polarity = TIM_ICPOLARITY_RISING;
    enc.IC2Selection = TIM_ICSELECTION_DIRECTTI;
    enc.IC2Prescaler = TIM_ICPSC_DIV1;
    enc.IC2Filter = 0u;
    (void)HAL_TIM_Encoder_Init(htim, &enc);
    (void)HAL_TIM_Encoder_Start(htim, TIM_CHANNEL_ALL);
}

static void pid_timer_init(void)
{
    __HAL_RCC_TIM7_CLK_ENABLE();

    htim_pid.Instance = DRIVE_PID_TIM;
    htim_pid.Init.Prescaler = 0u; /* adjust once the real clock tree is set, for DRIVE_PID_RATE_HZ */
    htim_pid.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim_pid.Init.Period = 0u;
    htim_pid.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim_pid.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    (void)HAL_TIM_Base_Init(&htim_pid);

    HAL_NVIC_SetPriority(DRIVE_PID_TIM_IRQ, 1u, 0u);
    HAL_NVIC_EnableIRQ(DRIVE_PID_TIM_IRQ);
    (void)HAL_TIM_Base_Start_IT(&htim_pid);
}

static void adc_init(void)
{
    __HAL_RCC_ADC1_CLK_ENABLE();

    hadc1.Instance = DRIVE_CURRENT_ADC;
    hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
    hadc1.Init.Resolution = ADC_RESOLUTION_12B;
    hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
    hadc1.Init.ScanConvMode = DISABLE;
    hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
    hadc1.Init.ContinuousConvMode = DISABLE;
    hadc1.Init.NbrOfConversion = 1u;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
    (void)HAL_ADC_Init(&hadc1);
}

static void iwdg_init(void)
{
    hiwdg.Instance = IWDG;
    hiwdg.Init.Prescaler = IWDG_PRESCALER_32;
    /* LSI ~32 kHz / 32 = 1 kHz counter; DRIVE_IWDG_TIMEOUT_MS ticks to the reload value. */
    hiwdg.Init.Reload = DRIVE_IWDG_TIMEOUT_MS;
    (void)HAL_IWDG_Init(&hiwdg);
}

void board_hw_init(void)
{
    board_crash_init(); /* first: read the reset flags before anything else can reset the chip */
    s_board_hooks.ctx = NULL;
    s_board_hooks.read_node_id = board_read_node_id;
    s_board_hooks.set_outputs_off = board_set_outputs_off;
    s_board_hooks.feed_hw_watchdog = board_feed_hw_watchdog;
    s_board_hooks.now_ms = board_now_ms;
    s_board_hooks.enter_bootloader = NULL; /* bootloader entry: out of scope (CLAUDE.md A2) */
    s_board_hooks.request_reset = NULL;
    s_board_hooks.storage_load = NULL; /* flash-backed parameter storage: left for a later prompt */
    s_board_hooks.storage_save = NULL;
    s_board_hooks.log_load = board_log_load;
    s_board_hooks.log_save = board_log_save;
    s_board_hooks.take_crash_record = board_crash_take;
    s_board_hooks.reset_cause = board_crash_reset_cause;
    s_board_hooks.self_test = board_self_test;

    s_motor_hw.ctx = NULL;
    s_motor_hw.set_duty = motor_set_duty;
    s_motor_hw.set_coast = motor_set_coast;
    s_motor_hw.set_brake = motor_set_brake;
    s_motor_hw.read_encoder = motor_read_encoder;
    s_motor_hw.read_current_ma = motor_read_current_ma;

    s_telem_hw.ctx = NULL;
    s_telem_hw.read_supply_mv = telem_supply_mv;
    s_telem_hw.read_temp_driver_a = telem_temp_stub;
    s_telem_hw.read_temp_driver_b = telem_temp_stub;
    s_telem_hw.read_temp_mcu = telem_temp_stub;

    gpio_init();
    pwm_init();
    __HAL_RCC_TIM2_CLK_ENABLE();
    __HAL_RCC_TIM3_CLK_ENABLE();
    encoder_init_one(&htim_enc_a, DRIVE_MOTOR_A_ENCODER_TIM);
    encoder_init_one(&htim_enc_b, DRIVE_MOTOR_B_ENCODER_TIM);
    pid_timer_init();
    adc_init();
    iwdg_init();
}

void TIM7_IRQHandler(void)
{
    __HAL_TIM_CLEAR_IT(&htim_pid, TIM_IT_UPDATE);
    board_hw_pid_tick();
}

void CAN1_RX0_IRQHandler(void)
{
    HAL_CAN_IRQHandler(&hcan1);
}

void CAN1_SCE_IRQHandler(void)
{
    HAL_CAN_IRQHandler(&hcan1);
}
