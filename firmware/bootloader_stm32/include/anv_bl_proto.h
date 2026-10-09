/* Bootloader wire protocol: the request/response payloads carried over ISO-TP on class 7
 * (sub 0 host -> node, sub 1 node -> host), and the application image header.
 * docs/architecture.md, "Firmware updates over CAN".
 *
 * host/tools/anv_flash/anv_flash/protocol.py mirrors every ANV_BL_* constant below; its test
 * suite parses this header to keep the two in step, so change both together.
 *
 * Every response starts with the same 4 bytes: [command][status][error code, u16 LE]. The
 * error code is the defs/errors.yaml code behind a failure (0 on success), so a host can show
 * the same name and description the diagnostics pipeline uses. Multi-byte fields are
 * little-endian. Offsets are relative to the start of the application region (the header
 * slot), so the protocol is the same whatever the flash layout.
 */
#ifndef ANV_BL_PROTO_H
#define ANV_BL_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANV_BL_VERSION 0x0100u /* major << 8 | minor */

/* ---- Commands (first byte of every request and response) ------------------------------- */
#define ANV_BL_CMD_INFO 0x01u   /* -> INFO payload below */
#define ANV_BL_CMD_ERASE 0x02u  /* [offset u32][length u32] */
#define ANV_BL_CMD_WRITE 0x03u  /* [offset u32][data, 1..ANV_BL_MAX_WRITE bytes] */
#define ANV_BL_CMD_VERIFY 0x04u /* [expected CRC32 of the image body u32] -> [actual CRC32 u32] */
#define ANV_BL_CMD_COMMIT 0x05u /* writes the stashed header last */
#define ANV_BL_CMD_BOOT 0x06u   /* replies, then resets into the application */
#define ANV_BL_CMD_ABORT 0x07u  /* erases the partial image, stays in the bootloader */
/* [uid32 u32] -> [node id u8][uid32 u32]. Only the board whose 32-bit UID matches replies;
 * every other board stays silent. Required before anything else on node 31 (unconfigured),
 * where several boards may share one channel; also how `anv-flash rescue --uid` finds a
 * board whatever its node ID. */
#define ANV_BL_CMD_SELECT 0x08u

/* ---- Status codes (second byte of every response) -------------------------------------- */
#define ANV_BL_ST_OK 0u
#define ANV_BL_ST_MISMATCH 1u        /* VERIFY: CRC differs */
#define ANV_BL_ST_BAD_LENGTH 2u      /* request too short/long for the command */
#define ANV_BL_ST_BAD_RANGE 3u       /* offset/length outside the region or misaligned */
#define ANV_BL_ST_NOT_ERASED 4u      /* WRITE/COMMIT target not erased first */
#define ANV_BL_ST_FLASH_ERROR 5u     /* erase/program failed or read-back differs */
#define ANV_BL_ST_WRONG_NODE_TYPE 6u /* image header names another node type */
#define ANV_BL_ST_BAD_IMAGE 7u       /* header malformed, or image invalid at COMMIT/BOOT */
#define ANV_BL_ST_BAD_SEQUENCE 8u    /* e.g. VERIFY before the header, COMMIT before VERIFY */
#define ANV_BL_ST_UNKNOWN_COMMAND 9u

#define ANV_BL_RESP_PREFIX_LEN 4u

/* ---- Limits ----------------------------------------------------------------------------- */
#define ANV_BL_MAX_WRITE 4096u /* data bytes per WRITE ("up to 4 KB") */
#define ANV_BL_WRITE_OVERHEAD 5u /* command + offset */
/* Largest request: a full WRITE. ISO-TP sends anything over 4095 bytes with the 32-bit
 * first-frame length (ISO 15765-2:2016), which both isotp-c and can-isotp support. */
#define ANV_BL_MAX_REQUEST (ANV_BL_MAX_WRITE + ANV_BL_WRITE_OVERHEAD)
#define ANV_BL_MAX_LAYOUT_GROUPS 4u

/* ---- Application image header ----------------------------------------------------------- *
 * The first ANV_BL_HEADER_LEN bytes of the application region; the application's vector
 * table starts ANV_BL_HEADER_SLOT bytes in. The slot is 512 B rather than the doc's 256 B:
 * a Cortex-M4 vector table must be aligned to its size rounded up to a power of two, and the
 * F446 (113 entries) and G431 (118 entries) tables both need 512-byte alignment, so an
 * application at "+0x100" could not point VTOR at its own table.
 *
 * Layout (little-endian):
 *   0  magic u32 (ANV_BL_HEADER_MAGIC)   4 header_version u16   6 node_type u8   7 reserved u8
 *   8  image_length u32 (bytes after the slot)   12 image_crc32 u32 (CRC-32/ISO-HDLC, as zlib)
 *  16  fw_version u32 (major<<16 | minor<<8 | patch)   20 build_id u32 (NODE_HB shows low 16)
 *  24  git_hash char[16] (short hash, NUL-padded)   40 reserved[20] (zero)
 *  60  header_crc32 u32 over bytes 0..59
 * The build stamps image_length, image_crc32, build_id, git_hash and header_crc32 after
 * linking (host/tools/anv_flash/anv_flash/image.py). */
#define ANV_BL_HEADER_MAGIC 0x48564E41u /* "ANVH" */
#define ANV_BL_HEADER_VERSION 1u
#define ANV_BL_HEADER_LEN 64u
#define ANV_BL_HEADER_SLOT 0x200u
#define ANV_BL_HEADER_CRC_OFFSET 60u

typedef struct {
    uint32_t magic;
    uint16_t header_version;
    uint8_t node_type;
    uint8_t reserved0;
    uint32_t image_length;
    uint32_t image_crc32;
    uint32_t fw_version;
    uint32_t build_id;
    char git_hash[16];
    uint8_t reserved1[20];
    uint32_t header_crc32;
} anv_bl_app_header_t;

/* ---- "Stay in bootloader" flag ---------------------------------------------------------- *
 * Written to backup register 0 by the application on ENTER_BOOTLOADER, read and cleared by
 * the bootloader on the next reset. */
#define ANV_BL_STAY_FLAG_MAGIC 0x424C5354u /* "BLST" */

/* ---- INFO response payload (after the 4-byte prefix) ------------------------------------ *
 *  0 bl_version u16   2 node_id u8   3 node_type u8   4 uid[12]   16 uid32 u32
 * 20 flash_size_kb u16   22 app_base u32 (absolute address of the header slot)
 * 26 region_size u32   30 header_slot u16   32 write_align u8   33 max_write u16
 * 35 boot_status u16 (0 = application valid, else the errors.yaml code why not)
 * 37 n_groups u8, then n_groups x {count u16, size u32}: the erase units of the region, in
 *    order (e.g. G431: {46, 2048}; F446: {1, 65536}, {3, 131072})
 * then the 64 header bytes currently in flash (all 0xFF if none). */
#define ANV_BL_INFO_FIXED_LEN 38u

#ifdef __cplusplus
}
#endif

#endif /* ANV_BL_PROTO_H */
