#include "anv_syncbuf.h"

#include <stddef.h>
#include <string.h>

void anv_sync_buf_init(anv_sync_buf_t *buf)
{
    memset(buf, 0, sizeof(*buf));
}

void anv_sync_buf_store(anv_sync_buf_t *buf, const uint8_t *data, uint8_t len)
{
    if (len > ANV_SYNC_BUF_MAX_LEN) {
        len = ANV_SYNC_BUF_MAX_LEN;
    }
    memcpy(buf->pending, data, len);
    buf->pending_len = len;
    buf->pending_valid = true;
}

void anv_sync_buf_apply(anv_sync_buf_t *buf)
{
    if (!buf->pending_valid) {
        return;
    }
    memcpy(buf->active, buf->pending, buf->pending_len);
    buf->active_len = buf->pending_len;
    buf->active_valid = true;
}

const uint8_t *anv_sync_buf_active(const anv_sync_buf_t *buf, uint8_t *len_out)
{
    if (!buf->active_valid) {
        if (len_out != NULL) {
            *len_out = 0;
        }
        return NULL;
    }
    if (len_out != NULL) {
        *len_out = buf->active_len;
    }
    return buf->active;
}
