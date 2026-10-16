/* Tracks which host is currently master from HOST_HB frames (docs/architecture.md, "Hosts:
 * choosing which one leads"). The node trusts the highest term it has seen; a term or
 * identity change means every node disarms. */
#ifndef ANV_MASTER_H
#define ANV_MASTER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Matches the ROLE signal in HOST_HB. */
typedef enum {
    ANV_HOST_ROLE_BOOTING = 0,
    ANV_HOST_ROLE_STANDBY = 1,
    ANV_HOST_ROLE_MASTER = 2,
    ANV_HOST_ROLE_OBSERVER = 3,
} anv_host_role_t;

typedef struct {
    bool have_master;
    uint8_t master_node_id;
    uint16_t master_term;
    uint32_t last_master_hb_ms;
} anv_master_tracker_t;

void anv_master_init(anv_master_tracker_t *tracker);

/* Feed every HOST_HB frame seen, from any host. `now_ms` is used for the "no master" timeout.
 * Returns true if the current master's identity (node ID or term) just changed -- the caller
 * must then disarm (docs/architecture.md: "When the term changes, every node disarms"). A
 * node's very first master acquisition is not reported as a change: the node starts DISARMED
 * already and only arms on an explicit ARM. */
bool anv_master_on_host_hb(anv_master_tracker_t *tracker, uint8_t sender_node_id, anv_host_role_t role,
                            uint16_t term, uint32_t now_ms);

/* True if `sender_node_id` is the current master and its term's low bits (masked by
 * `term_mask`) match `term_bits`. Use term_mask = 0x03 for DRIVE_CMD's 2-bit term field, or
 * 0xFF for NMT/ESTOP_RELEASE's full low byte. */
bool anv_master_check(const anv_master_tracker_t *tracker, uint8_t sender_node_id, uint8_t term_bits,
                       uint8_t term_mask);

/* True if no host has announced itself as master for at least `timeout_ms` (300 ms per spec). */
bool anv_master_lost(const anv_master_tracker_t *tracker, uint32_t now_ms, uint32_t timeout_ms);

uint8_t anv_master_node_id(const anv_master_tracker_t *tracker);
uint16_t anv_master_term(const anv_master_tracker_t *tracker);

#ifdef __cplusplus
}
#endif

#endif /* ANV_MASTER_H */
