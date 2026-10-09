#include "app_image.h"

#include "anv_bl_proto.h"
#include "anv_bl_stm32.h"
#include "drive_app.h"

#ifndef DRIVE_FW_VERSION
#define DRIVE_FW_VERSION 0x00010000u /* major << 16 | minor << 8 | patch */
#endif

/* Fields the build stamps after linking (length, CRCs, build ID, git hash) are left erased
 * (0xFF) here; host/tools/anv_flash/anv_flash/image.py fills them in. */
__attribute__((section(".app_header"), used)) const anv_bl_app_header_t g_app_header = {
    .magic = ANV_BL_HEADER_MAGIC,
    .header_version = ANV_BL_HEADER_VERSION,
    .node_type = DRIVE_NODE_TYPE,
    .reserved0 = 0u,
    .image_length = 0xFFFFFFFFu,
    .image_crc32 = 0xFFFFFFFFu,
    .fw_version = DRIVE_FW_VERSION,
    .build_id = 0xFFFFFFFFu,
    .git_hash = {(char)0xFF, (char)0xFF, (char)0xFF, (char)0xFF, (char)0xFF, (char)0xFF, (char)0xFF, (char)0xFF,
                 (char)0xFF, (char)0xFF, (char)0xFF, (char)0xFF, (char)0xFF, (char)0xFF, (char)0xFF, (char)0xFF},
    .reserved1 = {0},
    .header_crc32 = 0xFFFFFFFFu,
};

uint32_t app_image_build_id(void)
{
    /* volatile: the compiler must not fold in the 0xFFFFFFFF placeholder above. */
    const volatile anv_bl_app_header_t *hdr = &g_app_header;
    return hdr->build_id;
}

void app_image_enter_bootloader(void *ctx)
{
    (void)ctx;
    anv_bl_stay_flag_set();
}

void app_image_request_reset(void *ctx)
{
    (void)ctx;
    NVIC_SystemReset();
}

void SysTick_Handler(void)
{
    HAL_IncTick();
}
