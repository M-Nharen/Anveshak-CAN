/* Stores the latest command and applies it only at the next SYNC, so every node changes
 * setpoints at the same instant even though commands arrive one after another
 * (docs/architecture.md, "The 20 ms control cycle"). Payload bytes are opaque to the core;
 * the application unpacks them with the generated code for its own command message. */
#ifndef ANV_SYNCBUF_H
#define ANV_SYNCBUF_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANV_SYNC_BUF_MAX_LEN 8u

typedef struct {
    uint8_t pending[ANV_SYNC_BUF_MAX_LEN];
    uint8_t pending_len;
    bool pending_valid;
    uint8_t active[ANV_SYNC_BUF_MAX_LEN];
    uint8_t active_len;
    bool active_valid;
} anv_sync_buf_t;

void anv_sync_buf_init(anv_sync_buf_t *buf);

/* Store a newly received command frame; overwrites whatever was pending. */
void anv_sync_buf_store(anv_sync_buf_t *buf, const uint8_t *data, uint8_t len);

/* Call on SYNC: move pending into active. A no-op if nothing has been stored yet. */
void anv_sync_buf_apply(anv_sync_buf_t *buf);

/* Returns the active command, or NULL if none has been applied yet. */
const uint8_t *anv_sync_buf_active(const anv_sync_buf_t *buf, uint8_t *len_out);

#ifdef __cplusplus
}
#endif

#endif /* ANV_SYNCBUF_H */
