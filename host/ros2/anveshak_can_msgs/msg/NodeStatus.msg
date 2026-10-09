# One entry of the node registry.
uint8 node_id
string name
bool seen               # at least one frame since the gateway started
bool alive              # a frame within the last 300 ms
uint8 state             # see WheelFeedback.STATE_*
uint8 node_type
uint8 protocol_version
uint8 can_error_state   # 0 active, 1 passive, 2 recovered from bus-off
bool bootloader_running
bool unsaved_param_changes
uint8 debug_level
uint16 fw_build_id      # low 16 bits of the git commit hash
uint16 uptime_s
uint32 reboot_count     # times the uptime went backwards
uint8 tec               # CAN error counters from the node's telemetry
uint8 rec
