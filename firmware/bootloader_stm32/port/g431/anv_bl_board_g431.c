/* STM32G431 (FDCAN, classic mode) bootloader board support: clocks, CAN, and the page-based
 * flash backend. Flash map (docs/architecture.md, "STM32 flash layout", 2 KB-page column):
 *   0x0800_0000 pages 0-15 (32 KB)  bootloader, write-protected
 *   0x0800_8000 pages 16-17 (4 KB)  parameters
 *   0x0800_9000 pages 18..          header slot + application
 */
#include "acc_can_fdcan.h"
#include "anv_bl_board.h"
#include "anv_bl_proto.h"
#include "board_config.h"

#include <string.h>

#define APP_REGION_BASE 0x08009000u
#define PAGE_SIZE 2048u
#define FIRST_APP_PAGE ((APP_REGION_BASE - FLASH_BASE) / PAGE_SIZE)

static FDCAN_HandleTypeDef s_hfdcan;
static acc_can_fdcan_ctx_t s_can_ctx;
static acc_can_driver_t s_can;

static anv_bl_erase_group_t s_group;
static anv_bl_flash_t s_flash;

/* ---- Clocks ----------------------------------------------------------------------------- */

/* From a 24 MHz HSE (HSE_VALUE in the board's stm32g4xx_hal_conf.h): VCO 320 MHz, SYSCLK
 * 80 MHz (no boost mode needed) and PLLQ 160 MHz as the FDCAN kernel clock, the value
 * acc_can_fdcan_timing.h expects. */
static void clock_init(void)
{
    (void)HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);

    RCC_OscInitTypeDef osc = {0};
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState = RCC_HSE_ON;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM = RCC_PLLM_DIV6; /* 4 MHz PLL input */
    osc.PLL.PLLN = 80u;           /* 320 MHz VCO */
    osc.PLL.PLLP = RCC_PLLP_DIV8;
    osc.PLL.PLLQ = RCC_PLLQ_DIV2; /* 160 MHz: FDCAN */
    osc.PLL.PLLR = RCC_PLLR_DIV4; /* 80 MHz: SYSCLK */
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        return; /* stays on HSI: CAN timing will be wrong, but nothing unsafe happens */
    }

    RCC_ClkInitTypeDef clk = {0};
    clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    (void)HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2);

    RCC_PeriphCLKInitTypeDef periph = {0};
    periph.PeriphClockSelection = RCC_PERIPHCLK_FDCAN;
    periph.FdcanClockSelection = RCC_FDCANCLKSOURCE_PLL;
    (void)HAL_RCCEx_PeriphCLKConfig(&periph);
}

void anv_bl_board_init(void)
{
    clock_init();

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = DRIVE_CAN_TX_PIN | DRIVE_CAN_RX_PIN;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = DRIVE_CAN_GPIO_AF;
    HAL_GPIO_Init(DRIVE_CAN_GPIO_PORT, &gpio);

    /* TCAN1044AV STB low: transceiver out of standby. */
    HAL_GPIO_WritePin(DRIVE_TCAN_STB_PORT, DRIVE_TCAN_STB_PIN, GPIO_PIN_RESET);
    gpio.Pin = DRIVE_TCAN_STB_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_PULLDOWN;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = 0u;
    HAL_GPIO_Init(DRIVE_TCAN_STB_PORT, &gpio);

    gpio.Pin = DRIVE_NODE_ID_PIN0 | DRIVE_NODE_ID_PIN1 | DRIVE_NODE_ID_PIN2 | DRIVE_NODE_ID_PIN3 | DRIVE_NODE_ID_PIN4;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(DRIVE_NODE_ID_PORT, &gpio);
}

/* ---- CAN -------------------------------------------------------------------------------- */

void FDCAN1_IT0_IRQHandler(void)
{
    HAL_FDCAN_IRQHandler(&s_hfdcan);
}

void FDCAN1_IT1_IRQHandler(void)
{
    HAL_FDCAN_IRQHandler(&s_hfdcan);
}

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

const acc_can_driver_t *anv_bl_board_can_start(const acc_can_filter_t filters[2])
{
    __HAL_RCC_FDCAN_CLK_ENABLE();
    HAL_NVIC_SetPriority(FDCAN1_IT0_IRQn, 1u, 0u);
    HAL_NVIC_EnableIRQ(FDCAN1_IT0_IRQn);
    HAL_NVIC_SetPriority(FDCAN1_IT1_IRQn, 1u, 0u);
    HAL_NVIC_EnableIRQ(FDCAN1_IT1_IRQn);

    s_hfdcan.Instance = FDCAN1;
    s_can = acc_can_fdcan_make_driver(&s_can_ctx, &s_hfdcan);
    (void)s_can.init(s_can.ctx, 500000u, filters, 2u);
    return &s_can;
}

/* ---- Flash: pages 18.. ---------------------------------------------------------------- */

/* The ART data cache can hold pre-erase/pre-program contents; the read-back checks must see
 * the flash itself. */
static void flush_data_cache(void)
{
    __HAL_FLASH_DATA_CACHE_DISABLE();
    __HAL_FLASH_DATA_CACHE_RESET();
    __HAL_FLASH_DATA_CACHE_ENABLE();
}

static int flash_erase_unit(void *ctx, uint32_t index)
{
    (void)ctx;
    FLASH_EraseInitTypeDef erase = {0};
    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.Banks = FLASH_BANK_1;
    erase.Page = FIRST_APP_PAGE + index;
    erase.NbPages = 1u;
    uint32_t bad_page = 0u;

    (void)HAL_FLASH_Unlock();
    HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&erase, &bad_page);
    (void)HAL_FLASH_Lock();
    flush_data_cache();
    return (st == HAL_OK && bad_page == 0xFFFFFFFFu) ? 0 : -1;
}

static int flash_program(void *ctx, uint32_t offset, const uint8_t *data, uint32_t len)
{
    (void)ctx;
    int rc = 0;
    (void)HAL_FLASH_Unlock();
    for (uint32_t i = 0; i < len; i += 8u) {
        uint64_t dword;
        memcpy(&dword, &data[i], sizeof(dword)); /* request buffers are not aligned */
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, APP_REGION_BASE + offset + i, dword) != HAL_OK) {
            rc = -1;
            break;
        }
    }
    (void)HAL_FLASH_Lock();
    flush_data_cache();
    return rc;
}

static void flash_read(void *ctx, uint32_t offset, uint8_t *out, uint32_t len)
{
    (void)ctx;
    memcpy(out, (const void *)(APP_REGION_BASE + offset), len);
}

const anv_bl_flash_t *anv_bl_board_flash(uint16_t flash_size_kb)
{
    uint32_t total = (uint32_t)flash_size_kb * 1024u;
    s_group.count = (uint16_t)((total - (APP_REGION_BASE - FLASH_BASE)) / PAGE_SIZE);
    s_group.size = PAGE_SIZE;

    s_flash.ctx = NULL;
    s_flash.base_addr = APP_REGION_BASE;
    s_flash.region_size = (uint32_t)s_group.count * PAGE_SIZE;
    s_flash.write_align = 8u;
    s_flash.groups = &s_group;
    s_flash.n_groups = 1u;
    s_flash.erase_unit = flash_erase_unit;
    s_flash.program = flash_program;
    s_flash.read = flash_read;
    return &s_flash;
}
