/* Application image checks: header parsing and the CRC32 used for both the image body and
 * the header itself. The same checks run at boot (is there a valid application to jump to?)
 * and during flashing (is this image for this board?). */
#ifndef ANV_BL_IMAGE_H
#define ANV_BL_IMAGE_H

#include <stdint.h>

#include "anv_bl_flash.h"
#include "anv_bl_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CRC-32/ISO-HDLC (zlib.crc32, poly 0xEDB88320 reflected). Start with crc = 0 and feed
 * chunks in order: anv_bl_crc32(anv_bl_crc32(0, a, n), b, m) == crc32 of a||b. */
uint32_t anv_bl_crc32(uint32_t crc, const uint8_t *data, uint32_t len);

void anv_bl_header_decode(const uint8_t raw[ANV_BL_HEADER_LEN], anv_bl_app_header_t *hdr);

/* Checks a header on its own: magic, version, header CRC, node type and that the image fits
 * in a region of `region_size` bytes. Returns 0 if valid, else the errors.yaml code:
 * ROVER_ERR_WRONG_NODE_TYPE, or ROVER_ERR_APP_CRC_FAIL for anything else. */
uint16_t anv_bl_header_check(const anv_bl_app_header_t *hdr, uint8_t node_type, uint32_t region_size);

/* CRC32 of `len` bytes of flash starting at `offset`. */
uint32_t anv_bl_flash_crc32(const anv_bl_flash_t *flash, uint32_t offset, uint32_t len);

/* Full boot check of what is in flash: header (as above) plus the image CRC. Returns 0 if the
 * application may be started, else the errors.yaml code. `hdr` (may be NULL) receives the
 * decoded header. */
uint16_t anv_bl_app_check(const anv_bl_flash_t *flash, uint8_t node_type, anv_bl_app_header_t *hdr);

#ifdef __cplusplus
}
#endif

#endif /* ANV_BL_IMAGE_H */
