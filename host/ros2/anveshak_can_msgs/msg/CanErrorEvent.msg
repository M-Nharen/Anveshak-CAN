# /can/errors: the gateway's numbered event stream (docs: "Error reporting and diagnostics").
# Every NODE_EVENT the nodes report, plus the gateway's own 0x80xx codes, come through here with
# a severity and an error code any dashboard or script can look up in defs/errors.yaml.
uint8 SEVERITY_INFO=0
uint8 SEVERITY_WARNING=1
uint8 SEVERITY_ERROR=2
uint8 SEVERITY_CRITICAL=3

builtin_interfaces/Time stamp
string type          # e.g. NODE_EVENT, EVENT_GAP_DETECTED, GATEWAY_LINK_LOST, DEBUG_LEVEL_CHANGED
uint8 node_id         # the node, or host, this event is about (0 if not applicable)
uint16 code           # errors.yaml code, 0 if this event has none
uint8 severity        # valid when code != 0
uint16 detail         # meaning depends on code/type
bool cleared          # edge-triggered "condition cleared"
