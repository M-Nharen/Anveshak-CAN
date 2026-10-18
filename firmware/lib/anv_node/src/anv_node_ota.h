/* anv_node OTA: receive a firmware image over the schema's `transfer` port (transfer_service
 * `ota`) into the other OTA slot, verify it and reboot. Only allowed while the node isn't
 * armed. Flash access is behind anv_node_ota_backend_t, so the state machine here is the same
 * on Linux (tested with a fake backend) and on the ESP32 (anv_node_ota_esp32_backend()).
 *
 * Wire format, after ISO-TP reassembly (the first byte, the transfer_service itself, is
 * stripped by anv_node before it reaches here):
 *
 *   BEGIN (host -> node, 12 bytes): msg_type=0, type_id(u16), chip(u8), image_len(u32), crc32(u32)
 *   DATA  (host -> node): msg_type=1, offset(u32), then up to ANV_NODE_OTA_MAX_CHUNK bytes
 *   END   (host -> node, 1 byte): msg_type=2 -- verify the CRC, apply and reboot
 *   ABORT (host -> node, 1 byte): msg_type=3 -- give up, stay on the running image
 *   ACK   (node -> host, 6 bytes): msg_type=4, status (anv_node_ota_status_t), next_offset(u32)
 *
 * A BEGIN, DATA or END gets exactly one ACK. ABORT gets none (there is nothing left to tell).
 */
#ifndef ANV_NODE_OTA_H
#define ANV_NODE_OTA_H

#include <stdbool.h>
#include <stdint.h>

#include "anv_proto.h" /* anv_node_id_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Chunk payload the node will accept in one DATA message (host may send less). Bounded by
 * ANV_ISOTP_MAX_LEN (4095) minus this protocol's 6-byte DATA header. */
#define ANV_NODE_OTA_MAX_CHUNK 2048u

typedef enum {
    ANV_NODE_OTA_STATUS_OK = 0,
    ANV_NODE_OTA_STATUS_BAD_TYPE = 1,    /* type_id or chip doesn't match this node */
    ANV_NODE_OTA_STATUS_TOO_BIG = 2,     /* image_len won't fit the backend's slot */
    ANV_NODE_OTA_STATUS_NOT_DISARMED = 3,
    ANV_NODE_OTA_STATUS_BAD_OFFSET = 4,  /* DATA offset doesn't match what's expected */
    ANV_NODE_OTA_STATUS_WRITE_FAILED = 5,
    ANV_NODE_OTA_STATUS_CRC_MISMATCH = 6,
    ANV_NODE_OTA_STATUS_NO_SESSION = 7,  /* DATA/END/ABORT with no BEGIN first */
    ANV_NODE_OTA_STATUS_BUSY = 8,        /* a session with another peer is running */
    ANV_NODE_OTA_STATUS_NO_BACKEND = 9,
} anv_node_ota_status_t;

/* Flash access for one chip/platform. All of it may block briefly (flash writes); none of it
 * may be called from an ISR. NULL in anv_node_config_t: OTA always answers NO_BACKEND.
 *
 * Which image is "pending verify" is tracked by anv_node itself (through cfg.storage), not by
 * the backend: that keeps the rollback decision portable and testable without real flash. */
typedef struct {
    /* Start writing a new image of `image_len` bytes into the other slot. */
    bool (*begin)(void *ctx, uint32_t image_len);
    bool (*write)(void *ctx, const uint8_t *data, uint16_t len);
    /* The image is in and its CRC matched (checked by anv_node_ota itself, not the backend).
     * Make it the boot image; the caller reboots right after this returns true. */
    bool (*commit)(void *ctx);
    /* Stop a BEGIN/DATA sequence that didn't finish (CRC failed, ABORT, or a timeout). */
    void (*abort)(void *ctx);
    /* The running image is good: stop offering a rollback (best-effort; may be a no-op). */
    void (*confirm)(void *ctx);
    /* The running image didn't confirm in time: go back to the previous one and reboot right
     * away. Does not return on real hardware. */
    void (*rollback_and_reboot)(void *ctx);
} anv_node_ota_backend_t;

typedef enum { ANV_NODE_OTA_IDLE, ANV_NODE_OTA_RECEIVING } anv_node_ota_phase_t;

typedef struct {
    anv_node_ota_phase_t phase;
    anv_node_id_t peer;
    uint32_t image_len;
    uint32_t received;
    uint32_t crc32_expected;
    uint32_t crc32_running;
    uint32_t deadline_ms; /* session abandoned (back to IDLE) if nothing arrives by this time */
    bool confirmed;       /* this boot's image already called backend->confirm */
} anv_node_ota_t;

/* How long a session may sit idle between messages before anv_node gives up on it. */
#define ANV_NODE_OTA_SESSION_TIMEOUT_MS 5000u
/* How long a freshly booted, unconfirmed image gets to prove itself before anv_node calls
 * backend->confirm() on its own (it reached this point: the node runtime is alive and the
 * schema's heartbeat is going out). */
#define ANV_NODE_OTA_CONFIRM_MS 5000u

#ifdef __cplusplus
}
#endif

#endif /* ANV_NODE_OTA_H */
