/* STM32F446 (bxCAN) bootloader board support: clocks, CAN, and the sector-based flash
 * backend. Flash map (docs/architecture.md, "STM32 flash layout", F4 column):
 *   0x0800_0000 sectors 0-1 (2 x 16 KB)  bootloader, write-protected
 *   0x0800_8000 sectors 2-3 (2 x 16 KB)  parameters
 *   0x0801_0000 sector 4 (64 KB), sectors 5.. (128 KB)  header slot + application
 */
#include "acc_can_bxcan.h"
#include "anv_bl_board.h"
#include "anv_bl_proto.h"
#include "board_config.h"

#include <string.h>

#define APP_REGION_BASE 0x08010000u
#define FIRST_APP_SECTOR 4u

static CAN_HandleTypeDef s_hcan;
static acc_can_bxcan_ctx_t s_can_ctx;
static acc_can_driver_t s_can;

static anv_bl_erase_group_t s_groups[2];
static anv_bl_flash_t s_flash;

/* ---- Clocks ----------------------------------------------------------------------------- */

/* 90 MHz from an 8 MHz HSE (HSE_VALUE in the board's stm32f4xx_hal_conf.h): APB1 = 45 MHz,
 * the bxCAN clock acc_can_bxcan_timing.h expects. The bootloader doesn't need the full
 * 180 MHz, and staying at 90 MHz avoids over-drive mode. */
static void clock_init(void)
{
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    RCC_OscInitTypeDef osc = {0};
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState = RCC_HSE_ON;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM = HSE_VALUE / 1000000u; /* 1 MHz PLL input */
    osc.PLL.PLLN = 180u;                 /* 180 MHz VCO */
    osc.PLL.PLLP = RCC_PLLP_DIV2;        /* 90 MHz SYSCLK */
    osc.PLL.PLLQ = 4u;
    osc.PLL.PLLR = 2u;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        return; /* stays on HSI: CAN timing will be wrong, but nothing unsafe happens */
    }

    RCC_ClkInitTypeDef clk = {0};
    clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV2; /* 45 MHz */
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    (void)HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2);
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

void CAN1_RX0_IRQHandler(void)
{
    HAL_CAN_IRQHandler(&s_hcan);
}

void CAN1_SCE_IRQHandler(void)
{
    HAL_CAN_IRQHandler(&s_hcan);
}

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

const acc_can_driver_t *anv_bl_board_can_start(const acc_can_filter_t filters[2])
{
    __HAL_RCC_CAN1_CLK_ENABLE();
    HAL_NVIC_SetPriority(CAN1_RX0_IRQn, 1u, 0u);
    HAL_NVIC_EnableIRQ(CAN1_RX0_IRQn);
    HAL_NVIC_SetPriority(CAN1_SCE_IRQn, 1u, 0u);
    HAL_NVIC_EnableIRQ(CAN1_SCE_IRQn);

    s_hcan.Instance = CAN1;
    s_can = acc_can_bxcan_make_driver(&s_can_ctx, &s_hcan);
    (void)s_can.init(s_can.ctx, 500000u, filters, 2u);
    return &s_can;
}

/* ---- Flash: sectors 4.. --------------------------------------------------------------- */

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
    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Sector = FIRST_APP_SECTOR + index;
    erase.NbSectors = 1u;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3; /* 2.7-3.6 V: word programming allowed */
    uint32_t bad_sector = 0u;

    (void)HAL_FLASH_Unlock();
    HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&erase, &bad_sector);
    (void)HAL_FLASH_Lock();
    flush_data_cache();
    return (st == HAL_OK && bad_sector == 0xFFFFFFFFu) ? 0 : -1;
}

static int flash_program(void *ctx, uint32_t offset, const uint8_t *data, uint32_t len)
{
    (void)ctx;
    int rc = 0;
    (void)HAL_FLASH_Unlock();
    for (uint32_t i = 0; i < len; i += 4u) {
        uint32_t word;
        memcpy(&word, &data[i], sizeof(word)); /* request buffers are not word-aligned */
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, APP_REGION_BASE + offset + i, word) != HAL_OK) {
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
    /* 256 KB parts end after sector 5, 512 KB parts after sector 7. */
    uint32_t big_sectors = (flash_size_kb > 128u) ? ((uint32_t)flash_size_kb - 128u) / 128u : 0u;
    s_groups[0].count = 1u;
    s_groups[0].size = 64u * 1024u;
    s_groups[1].count = (uint16_t)big_sectors;
    s_groups[1].size = 128u * 1024u;

    s_flash.ctx = NULL;
    s_flash.base_addr = APP_REGION_BASE;
    s_flash.region_size = s_groups[0].size + big_sectors * s_groups[1].size;
    s_flash.write_align = 4u;
    s_flash.groups = s_groups;
    s_flash.n_groups = 2u;
    s_flash.erase_unit = flash_erase_unit;
    s_flash.program = flash_program;
    s_flash.read = flash_read;
    return &s_flash;
}
