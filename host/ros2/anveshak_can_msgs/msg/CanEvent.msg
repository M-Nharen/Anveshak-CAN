# /can/events: things the gateway noticed.
uint8 TYPE_ROLE_CHANGED=0
uint8 TYPE_LEADER_LOST=1
uint8 TYPE_NODE_ALIVE=2
uint8 TYPE_NODE_MISSING=3
uint8 TYPE_NODE_REBOOTED=4
uint8 TYPE_ESTOP_SEEN=5
uint8 TYPE_ESTOP_RELEASED=6

builtin_interfaces/Time stamp
uint8 type
string name             # e.g. NODE_MISSING
uint8 node_id           # subject: node, or host for role and E-stop events
uint16 term
uint8 role              # for ROLE_CHANGED
