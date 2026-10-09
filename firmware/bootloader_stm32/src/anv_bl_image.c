#include "anv_bl_image.h"

#include <string.h>

#include "errors.h"

_Static_assert(sizeof(anv_bl_app_header_t) == ANV_BL_HEADER_LEN, "header layout must be exactly 64 bytes");
_Static_assert(ANV_BL_HEADER_CRC_OFFSET == ANV_BL_HEADER_LEN - 4u, "header CRC is the last field");

/* Nibble table: 64 bytes of constants instead of 1 KB, and still ~4x faster than bitwise --
 * the boot check runs over the whole image on every reset. */
static const uint32_t k_crc_nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t anv_bl_crc32(uint32_t crc, const uint8_t *data, uint32_t len)
{
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        crc = (crc >> 4) ^ k_crc_nibble[crc & 0x0Fu];
        crc = (crc >> 4) ^ k_crc_nibble[crc & 0x0Fu];
    }
    return ~crc;
}

void anv_bl_header_decode(const uint8_t raw[ANV_BL_HEADER_LEN], anv_bl_app_header_t *hdr)
{
    /* Every supported target (Cortex-M and the x86/ARM hosts running the tests) is
     * little-endian, which is the wire/flash format, so the struct maps directly. */
    memcpy(hdr, raw, ANV_BL_HEADER_LEN);
}

uint16_t anv_bl_header_check(const anv_bl_app_header_t *hdr, uint8_t node_type, uint32_t region_size)
{
    if (hdr->magic != ANV_BL_HEADER_MAGIC || hdr->header_version != ANV_BL_HEADER_VERSION) {
        return ROVER_ERR_APP_CRC_FAIL;
    }
    if (anv_bl_crc32(0u, (const uint8_t *)hdr, ANV_BL_HEADER_CRC_OFFSET) != hdr->header_crc32) {
        return ROVER_ERR_APP_CRC_FAIL;
    }
    /* Checked after the header CRC so a corrupt byte is reported as corruption, not as a
     * wrong-type image. */
    if (hdr->node_type != node_type) {
        return ROVER_ERR_WRONG_NODE_TYPE;
    }
    if (hdr->image_length == 0u || region_size < ANV_BL_HEADER_SLOT ||
        hdr->image_length > region_size - ANV_BL_HEADER_SLOT) {
        return ROVER_ERR_APP_CRC_FAIL;
    }
    return 0u;
}

uint32_t anv_bl_flash_crc32(const anv_bl_flash_t *flash, uint32_t offset, uint32_t len)
{
    uint8_t chunk[64];
    uint32_t crc = 0u;
    while (len > 0u) {
        uint32_t n = (len < sizeof(chunk)) ? len : (uint32_t)sizeof(chunk);
        flash->read(flash->ctx, offset, chunk, n);
        crc = anv_bl_crc32(crc, chunk, n);
        offset += n;
        len -= n;
    }
    return crc;
}

uint16_t anv_bl_app_check(const anv_bl_flash_t *flash, uint8_t node_type, anv_bl_app_header_t *hdr)
{
    uint8_t raw[ANV_BL_HEADER_LEN];
    anv_bl_app_header_t local;
    anv_bl_app_header_t *h = (hdr != NULL) ? hdr : &local;

    flash->read(flash->ctx, 0u, raw, ANV_BL_HEADER_LEN);
    anv_bl_header_decode(raw, h);

    uint16_t err = anv_bl_header_check(h, node_type, flash->region_size);
    if (err != 0u) {
        return err;
    }
    if (anv_bl_flash_crc32(flash, ANV_BL_HEADER_SLOT, h->image_length) != h->image_crc32) {
        return ROVER_ERR_APP_CRC_FAIL;
    }
    return 0u;
}
