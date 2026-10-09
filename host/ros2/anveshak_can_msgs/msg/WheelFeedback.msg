# Latest decoded feedback of one drive node.
uint8 STATE_BOOT=0
uint8 STATE_DISARMED=1
uint8 STATE_ARMED=2
uint8 STATE_FAULT=3
uint8 STATE_ESTOP=4
uint8 STATE_BOOTLOADER=5

# fault_flags bits
uint8 FAULT_OVERCURRENT_A=1
uint8 FAULT_OVERCURRENT_B=2
uint8 FAULT_OVER_TEMP=4
uint8 FAULT_UNDER_VOLTAGE=8
uint8 FAULT_ENCODER_A=16
uint8 FAULT_ENCODER_B=32
uint8 FAULT_COMMAND_TIMEOUT=64
uint8 FAULT_ESTOP_LATCHED=128

uint8 drive            # 1-6
uint8 node_id          # CAN node ID (3 + drive)
bool alive             # a frame from this node within the last 300 ms
bool has_feedback      # at least one DRIVE_FB_A has arrived
uint8 state
uint8 fault_flags
float64 speed_a        # rad/s
float64 speed_b
float64 current_a      # A
float64 current_b
bool has_encoders
int32 encoder_a        # raw counts
int32 encoder_b
bool has_telemetry
float64 supply_voltage_v
int8 temp_driver_a     # degrees C
int8 temp_driver_b
int8 temp_mcu
float64 duty_a         # percent
float64 duty_b
