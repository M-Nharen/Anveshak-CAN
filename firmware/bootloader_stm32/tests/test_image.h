/* Builds application images (header slot + body) in memory, the same way the build's stamp
 * step does (host/tools/anv_flash/anv_flash/image.py). */
#ifndef TEST_IMAGE_H
#define TEST_IMAGE_H

#include <stdint.h>
#include <string.h>

#include "anv_bl_image.h"
#include "anv_bl_proto.h"

#define TEST_IMAGE_MAX (ANV_BL_HEADER_SLOT + 64u * 1024u)

typedef struct {
    uint8_t bytes[TEST_IMAGE_MAX];
    uint32_t len;
    anv_bl_app_header_t hdr;
} test_image_t;

static inline void test_image_seal_header(test_image_t *img)
{
    img->hdr.header_crc32 = anv_bl_crc32(0u, (const uint8_t *)&img->hdr, ANV_BL_HEADER_CRC_OFFSET);
    memcpy(img->bytes, &img->hdr, ANV_BL_HEADER_LEN);
}

/* `body_len` bytes of deterministic pseudo-random content, so two builds with different
 * seeds differ everywhere. */
static inline void test_image_build(test_image_t *img, uint8_t node_type, uint32_t body_len, uint32_t seed,
                                    uint32_t build_id)
{
    memset(img->bytes, 0xFF, sizeof(img->bytes));
    uint32_t x = seed * 2654435761u + 1u;
    for (uint32_t i = 0; i < body_len; i++) {
        x = x * 1103515245u + 12345u;
        img->bytes[ANV_BL_HEADER_SLOT + i] = (uint8_t)(x >> 16);
    }
    memset(&img->hdr, 0, sizeof(img->hdr));
    img->hdr.magic = ANV_BL_HEADER_MAGIC;
    img->hdr.header_version = ANV_BL_HEADER_VERSION;
    img->hdr.node_type = node_type;
    img->hdr.image_length = body_len;
    img->hdr.image_crc32 = anv_bl_crc32(0u, &img->bytes[ANV_BL_HEADER_SLOT], body_len);
    img->hdr.fw_version = 0x00010200u;
    img->hdr.build_id = build_id;
    memcpy(img->hdr.git_hash, "abc1234", 7);
    test_image_seal_header(img);
    img->len = ANV_BL_HEADER_SLOT + body_len;
}

#endif /* TEST_IMAGE_H */
