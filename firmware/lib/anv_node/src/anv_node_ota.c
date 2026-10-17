#include "anv_node_ota.h"

#include <string.h>

#include "anv_node_diag.h"
#include "anv_node_internal.h"

#define OTA_MARKER_KEY "anv_ota_st"
#define OTA_MSG_BEGIN 0u
#define OTA_MSG_DATA 1u
#define OTA_MSG_END 2u
#define OTA_MSG_ABORT 3u
#define OTA_MSG_ACK 4u

static bool due(uint32_t now, uint32_t at)
{
    return (int32_t)(now - at) >= 0;
}

/* CRC-32 (same polynomial as anv_node_settings.c's), fed incrementally: pass the previous
 * return value in as `crc` (0xFFFFFFFF to start); invert the final result to compare it. */
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (unsigned b = 0; b < 8u; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

static void send_ack(anv_node_t *n, uint8_t status, uint32_t next_offset)
{
    uint8_t body[7];
    body[0] = (uint8_t)ANV_TRANSFER_SERVICE_OTA;
    body[1] = OTA_MSG_ACK;
    body[2] = status;
    body[3] = (uint8_t)next_offset;
    body[4] = (uint8_t)(next_offset >> 8);
    body[5] = (uint8_t)(next_offset >> 16);
    body[6] = (uint8_t)(next_offset >> 24);
    (void)anv_isotp_send(&n->transfer, body, sizeof body, (uint32_t)n->now_ms * 1000u);
}

static void end_session(anv_node_t *n)
{
    n->ota.phase = ANV_NODE_OTA_IDLE;
    anv_node_i_refresh_state(n);
}

static void handle_begin(anv_node_t *n, anv_node_id_t src, const uint8_t *d, uint16_t len)
{
    if (len < 12u) {
        return;
    }
    if (n->ota.phase == ANV_NODE_OTA_RECEIVING && n->ota.peer != src) {
        send_ack(n, ANV_NODE_OTA_STATUS_BUSY, 0);
        return;
    }
    if (anv_node_i_armed(n)) {
        send_ack(n, ANV_NODE_OTA_STATUS_NOT_DISARMED, 0);
        return;
    }
    if (n->cfg.ota == NULL) {
        send_ack(n, ANV_NODE_OTA_STATUS_NO_BACKEND, 0);
        return;
    }
    const uint16_t type_id = (uint16_t)(d[1] | (d[2] << 8));
    const uint8_t chip = d[3];
    const uint32_t image_len =
        (uint32_t)d[4] | ((uint32_t)d[5] << 8) | ((uint32_t)d[6] << 16) | ((uint32_t)d[7] << 24);
    const uint32_t crc =
        (uint32_t)d[8] | ((uint32_t)d[9] << 8) | ((uint32_t)d[10] << 16) | ((uint32_t)d[11] << 24);
    if (type_id != n->cfg.type->type_id || chip != n->cfg.info.chip) {
        anv_node_event(n, ANV_EVENT_UPDATE_WRONG_TYPE, type_id);
        send_ack(n, ANV_NODE_OTA_STATUS_BAD_TYPE, 0);
        return;
    }
    if (image_len == 0u || !n->cfg.ota->begin(n->cfg.ota_ctx, image_len)) {
        send_ack(n, ANV_NODE_OTA_STATUS_TOO_BIG, 0);
        return;
    }
    n->ota.phase = ANV_NODE_OTA_RECEIVING;
    n->ota.peer = src;
    n->ota.image_len = image_len;
    n->ota.received = 0;
    n->ota.crc32_expected = crc;
    n->ota.crc32_running = 0xFFFFFFFFu;
    n->ota.deadline_ms = n->now_ms + ANV_NODE_OTA_SESSION_TIMEOUT_MS;
    anv_node_i_refresh_state(n);
    anv_node_event(n, ANV_EVENT_UPDATE_STARTED, 0);
    send_ack(n, ANV_NODE_OTA_STATUS_OK, 0);
}

static void handle_data(anv_node_t *n, anv_node_id_t src, const uint8_t *d, uint16_t len)
{
    if (n->ota.phase != ANV_NODE_OTA_RECEIVING || n->ota.peer != src) {
        send_ack(n, ANV_NODE_OTA_STATUS_NO_SESSION, 0);
        return;
    }
    if (len < 5u) {
        return;
    }
    const uint32_t offset =
        (uint32_t)d[1] | ((uint32_t)d[2] << 8) | ((uint32_t)d[3] << 16) | ((uint32_t)d[4] << 24);
    const uint8_t *chunk = d + 5;
    const uint16_t chunk_len = (uint16_t)(len - 5u);
    if (offset != n->ota.received) {
        send_ack(n, ANV_NODE_OTA_STATUS_BAD_OFFSET, n->ota.received);
        return;
    }
    if ((uint64_t)n->ota.received + chunk_len > n->ota.image_len) {
        n->cfg.ota->abort(n->cfg.ota_ctx);
        end_session(n);
        send_ack(n, ANV_NODE_OTA_STATUS_TOO_BIG, n->ota.received);
        return;
    }
    if (!n->cfg.ota->write(n->cfg.ota_ctx, chunk, chunk_len)) {
        n->cfg.ota->abort(n->cfg.ota_ctx);
        end_session(n);
        send_ack(n, ANV_NODE_OTA_STATUS_WRITE_FAILED, n->ota.received);
        return;
    }
    n->ota.crc32_running = crc32_update(n->ota.crc32_running, chunk, chunk_len);
    n->ota.received += chunk_len;
    n->ota.deadline_ms = n->now_ms + ANV_NODE_OTA_SESSION_TIMEOUT_MS;
    send_ack(n, ANV_NODE_OTA_STATUS_OK, n->ota.received);
}

static void handle_end(anv_node_t *n, anv_node_id_t src)
{
    if (n->ota.phase != ANV_NODE_OTA_RECEIVING || n->ota.peer != src) {
        send_ack(n, ANV_NODE_OTA_STATUS_NO_SESSION, 0);
        return;
    }
    if (n->ota.received != n->ota.image_len || ~n->ota.crc32_running != n->ota.crc32_expected) {
        n->cfg.ota->abort(n->cfg.ota_ctx);
        end_session(n);
        send_ack(n, ANV_NODE_OTA_STATUS_CRC_MISMATCH, n->ota.received);
        return;
    }
    if (!n->cfg.ota->commit(n->cfg.ota_ctx)) {
        n->cfg.ota->abort(n->cfg.ota_ctx);
        end_session(n);
        send_ack(n, ANV_NODE_OTA_STATUS_WRITE_FAILED, n->ota.received);
        return;
    }
    send_ack(n, ANV_NODE_OTA_STATUS_OK, n->ota.received);
    if (n->cfg.storage != NULL) {
        const uint8_t marker = 1;
        (void)n->cfg.storage->save(n->cfg.storage_ctx, OTA_MARKER_KEY, &marker, 1u);
    }
    end_session(n);
    if (n->cfg.on_reset != NULL) {
        n->cfg.on_reset(n, n->cfg.user); /* reboots the board; doesn't return on real hardware */
    }
}

static void handle_abort(anv_node_t *n, anv_node_id_t src)
{
    if (n->ota.phase == ANV_NODE_OTA_RECEIVING && n->ota.peer == src) {
        n->cfg.ota->abort(n->cfg.ota_ctx);
        end_session(n);
    }
}

void anv_node_i_ota_handle(anv_node_t *n, anv_node_id_t src, const uint8_t *d, uint16_t len)
{
    if (len < 1u) {
        return;
    }
    switch (d[0]) {
    case OTA_MSG_BEGIN: handle_begin(n, src, d, len); break;
    case OTA_MSG_DATA: handle_data(n, src, d, len); break;
    case OTA_MSG_END: handle_end(n, src); break;
    case OTA_MSG_ABORT: handle_abort(n, src); break;
    default: break;
    }
}

void anv_node_i_ota_poll(anv_node_t *n)
{
    if (n->ota.phase == ANV_NODE_OTA_RECEIVING && due(n->now_ms, n->ota.deadline_ms)) {
        if (n->cfg.ota != NULL) {
            n->cfg.ota->abort(n->cfg.ota_ctx);
        }
        end_session(n);
    }
    if (!n->ota.confirmed && n->cfg.ota != NULL && due(n->now_ms, n->start_ms + ANV_NODE_OTA_CONFIRM_MS)) {
        n->cfg.ota->confirm(n->cfg.ota_ctx);
        if (n->cfg.storage != NULL) {
            (void)n->cfg.storage->erase(n->cfg.storage_ctx, OTA_MARKER_KEY);
        }
        n->ota.confirmed = true;
        anv_node_event(n, ANV_EVENT_UPDATE_DONE, (uint16_t)n->cfg.info.fw_build);
    }
}

/* Called once from anv_node_begin(), after the node ID (and so its link filters) are set up:
 * decide whether the image now running needs to prove itself, or already failed to last time. */
void anv_node_i_ota_boot_check(anv_node_t *n)
{
    n->ota.phase = ANV_NODE_OTA_IDLE;
    n->ota.confirmed = true;
    if (n->cfg.storage == NULL) {
        return;
    }
    uint8_t marker = 0;
    if (n->cfg.storage->load(n->cfg.storage_ctx, OTA_MARKER_KEY, &marker, 1u) != 1u || marker == 0u) {
        return;
    }
    if (marker >= 2u) {
        /* Already booted once since the update and never confirmed: it didn't survive. */
        (void)n->cfg.storage->erase(n->cfg.storage_ctx, OTA_MARKER_KEY);
        anv_node_event(n, ANV_EVENT_UPDATE_ROLLBACK, 0);
        if (n->cfg.ota != NULL) {
            n->cfg.ota->rollback_and_reboot(n->cfg.ota_ctx); /* doesn't return on real hardware */
        }
        return;
    }
    /* marker == 1: this is the new image's first boot. Give it a chance, but remember that a
     * second boot still carrying this marker means it didn't make it. */
    marker = 2u;
    (void)n->cfg.storage->save(n->cfg.storage_ctx, OTA_MARKER_KEY, &marker, 1u);
    n->ota.confirmed = false;
}
