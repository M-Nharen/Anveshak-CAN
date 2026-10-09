/* NMT (network management) frame semantics: ARM, DISARM, RESET, ENTER_BOOTLOADER,
 * FAULT_RESET, targeted at one node or all nodes (docs/architecture.md, NMT layout). */
#ifndef ANV_NMT_H
#define ANV_NMT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ANV_NMT_CMD_ARM = 1,
    ANV_NMT_CMD_DISARM = 2,
    ANV_NMT_CMD_RESET = 3,
    ANV_NMT_CMD_ENTER_BOOTLOADER = 4,
    ANV_NMT_CMD_FAULT_RESET = 5,
} anv_nmt_command_t;

#define ANV_NMT_TARGET_ALL 0u

/* True if an NMT frame addressed to `target_node` (0 = all nodes) applies to this node. */
bool anv_nmt_targets_me(uint8_t target_node, uint8_t own_node_id);

#ifdef __cplusplus
}
#endif

#endif /* ANV_NMT_H */
