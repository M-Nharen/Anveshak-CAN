/* Bootloader entry point, shared by every STM32 family: brings the board up through
 * anv_bl_board.h, then runs anv_bl_poll() until it says jump or reset. */
#include "anv_bl.h"
#include "anv_bl_board.h"
#include "anv_bl_image.h"
#include "anv_bl_stm32.h"
#include "board_config.h"
#include "errors.h"
#include "ids.h"

#ifndef ANV_BL_NODE_TYPE
#define ANV_BL_NODE_TYPE 1u /* NODE_HB NODE_TYPE: 1 drive */
#endif

static anv_bl_t s_bl;

void SysTick_Handler(void)
{
    HAL_IncTick();
}

/* docs/architecture.md, "Node ID and status LEDs": an unreadable or zero switch means node 31. */
static uint8_t read_node_id(void)
{
    static const uint16_t pins[5] = {DRIVE_NODE_ID_PIN0, DRIVE_NODE_ID_PIN1, DRIVE_NODE_ID_PIN2, DRIVE_NODE_ID_PIN3,
                                     DRIVE_NODE_ID_PIN4};
    uint8_t id = 0u;
    for (uint8_t i = 0; i < 5u; i++) {
        if (HAL_GPIO_ReadPin(DRIVE_NODE_ID_PORT, pins[i]) != GPIO_PIN_RESET) {
            id |= (uint8_t)(1u << i);
        }
    }
    return (id == 0u) ? ROVER_NODE_UNCONFIGURED : id;
}

/* The application may have started the IWDG, and a reset does not stop it when the
 * hardware-watchdog option bit is set. Reloading a stopped IWDG is harmless. */
static void feed_watchdog(void *ctx)
{
    (void)ctx;
    IWDG->KR = 0xAAAAu;
}

int main(void)
{
    HAL_Init();
    anv_bl_board_init();
    bool stay = anv_bl_stay_flag_take();

    uint8_t node_id = read_node_id();
    acc_can_filter_t filters[2];
    anv_bl_can_filters(node_id, filters);
    const acc_can_driver_t *can = anv_bl_board_can_start(filters);

    uint16_t flash_kb = anv_bl_stm32_flash_size_kb();
    const anv_bl_flash_t *flash = anv_bl_board_flash(flash_kb);
    uint8_t uid[12];
    anv_bl_stm32_read_uid(uid);

    anv_bl_config_t cfg = {
        .node_id = node_id,
        .node_type = ANV_BL_NODE_TYPE,
        .driver = can,
        .flash = flash,
        .uid = uid,
        .flash_size_kb = flash_kb,
        .stay_flag = stay,
        .feed_watchdog = feed_watchdog,
        .feed_ctx = NULL,
    };
    anv_bl_init(&s_bl, &cfg, HAL_GetTick());

    for (;;) {
        feed_watchdog(NULL);
        acc_can_frame_t frame;
        while (can->receive(can->ctx, &frame)) {
            anv_bl_on_rx_frame(&s_bl, &frame);
        }

        anv_bl_mode_t mode = anv_bl_poll(&s_bl, HAL_GetTick());
        if (mode == ANV_BL_MODE_JUMP) {
            anv_bl_app_header_t hdr;
            (void)anv_bl_app_check(flash, ANV_BL_NODE_TYPE, &hdr);
            uint32_t vt = flash->base_addr + ANV_BL_HEADER_SLOT;
            if (anv_bl_stm32_vector_ok(vt, vt + hdr.image_length)) {
                anv_bl_stm32_jump(vt);
            }
            anv_bl_refuse_jump(&s_bl, ROVER_ERR_APP_CRC_FAIL, HAL_GetTick());
        } else if (mode == ANV_BL_MODE_RESET) {
            NVIC_SystemReset();
        }
    }
}
