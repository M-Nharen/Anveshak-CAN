# Measured vs expected rate of one node-originated frame ID.
uint16 can_id
uint8 node_id
float32 measured_hz
float32 expected_hz     # 0 = event-driven or not expected at the node's debug level
bool low                # expected > 0 and measured below 80 % of it
