# /can/bus: this host's view of the bus and of the leadership.
uint8 ROLE_BOOTING=0
uint8 ROLE_STANDBY=1
uint8 ROLE_MASTER=2
uint8 ROLE_OBSERVER=3

std_msgs/Header header
uint8 host_id
uint8 role
uint16 term
uint8 believed_master   # node ID this host believes leads, 0 = none
bool leader_lost
bool armed              # this gateway sent ARM since it last claimed the lead
bool estop_latched
bool bus_off
uint64 rx_frames
uint64 tx_frames
uint64 tx_failures
uint64 error_frames
uint64 undecoded_frames
float32 rx_fps          # frames per second over the last full second
uint64 cycles_sent
uint64 late_cycles      # cycles that started more than 2 ms after their deadline
uint32 worst_lateness_us
