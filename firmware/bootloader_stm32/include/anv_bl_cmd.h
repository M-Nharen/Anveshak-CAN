/* Bootloader command logic: one complete request in, one complete response out. No CAN, no
 * ISO-TP and no hardware here (only the flash backend), so the whole command set is unit-
 * tested on Linux with a fake flash. Protocol: anv_bl_proto.h. */
#ifndef ANV_BL_CMD_H
#define ANV_BL_CMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "anv_bl_flash.h"
#include "anv_bl_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Big enough for the INFO response with a full layout and the header. */
#define ANV_BL_MAX_RESPONSE 160u

typedef struct {
    const anv_bl_flash_t *flash;
    uint8_t node_id;
    uint8_t node_type;
    uint8_t uid[12];
    uint32_t uid32;
    uint16_t flash_size_kb;

    /* Fed around long erases (IWDG may already be running if the application started it). */
    void (*feed_watchdog)(void *ctx);
    void *feed_ctx;

    /* Session state. The header arrives with the first WRITE but is only programmed by
     * COMMIT, after VERIFY, so a half-written image never has a valid header. */
    bool have_header;
    bool verified;
    bool selected;
    bool boot_requested; /* BOOT accepted: reset once the response has gone out */
    uint8_t header_raw[ANV_BL_HEADER_LEN];
    uint32_t touched_end; /* highest offset erased or written this session (for ABORT) */
} anv_bl_cmd_t;

void anv_bl_cmd_init(anv_bl_cmd_t *c, const anv_bl_flash_t *flash, uint8_t node_id, uint8_t node_type,
                     const uint8_t uid[12], uint16_t flash_size_kb);

/* 32-bit board ID used by SELECT and `anv-flash rescue --uid`: CRC32 of the 96-bit UID. */
uint32_t anv_bl_uid32(const uint8_t uid[12]);

/* Handles one request. Returns the response length (0 = send nothing: a SELECT for another
 * board). `resp` must hold ANV_BL_MAX_RESPONSE bytes. */
size_t anv_bl_cmd_handle(anv_bl_cmd_t *c, const uint8_t *req, size_t req_len, uint8_t *resp);

#ifdef __cplusplus
}
#endif

#endif /* ANV_BL_CMD_H */
