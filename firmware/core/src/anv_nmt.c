#include "anv_nmt.h"

bool anv_nmt_targets_me(uint8_t target_node, uint8_t own_node_id)
{
    return target_node == ANV_NMT_TARGET_ALL || target_node == own_node_id;
}
