# /can/drive/feedback: all six wheels. A standby host publishes it too; role says who is in charge.
uint8 ROLE_BOOTING=0
uint8 ROLE_STANDBY=1
uint8 ROLE_MASTER=2
uint8 ROLE_OBSERVER=3

std_msgs/Header header
uint8 host_id
uint8 role
uint16 term
WheelFeedback[6] wheels
