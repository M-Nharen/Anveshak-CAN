# The CAN schema

Everything that goes on the bus is described here, in YAML, once. Code for the ESP32 firmware
(C), the hosts (Python, and C++ via `schema.json`) and tools (a DBC file) is generated from it,
so the two ends of a message can never disagree.

```
schema/
  protocol.yaml        what every node and host speaks: IDs, heartbeat, E-stop, parameters...
  types/*.yaml         one file per kind of node (examples/ holds examples)
  fragments/*.yaml     reusable pieces a type can `include`
  buses/*.yaml         optional: a concrete bus, only for making a DBC file
  generated/           made by `anv-schema gen`; committed; never edit by hand
    c/                 anv_schema.h, anv_protocol.h/.c, anv_<type>.h/.c, anv_all_types.h/.c
    schema.json        the whole schema, expanded, for programs that don't read YAML
```

The tool is the `anv_schema` Python package in `host/python/anv_schema`:

```sh
pip install -e host/python/anv_schema        # once; gives you the `anv-schema` command
anv-schema check                             # validate everything
anv-schema gen                               # regenerate schema/generated/ (commit the result)
anv-schema show                              # list node types
anv-schema show motor_controller             # one type: every message, bit, parameter, event
```

## Ideas in one minute

- A **node** is any board on the bus. A **node type** is what kind of board it is. Types know
  nothing about how many boards of them exist or what their IDs are: IDs are given out at run
  time, and every node announces its type in its heartbeat.
- Each node type declares four kinds of thing:
  - **messages** it sends (streams, events) or receives (commands);
  - **parameters**: settings a host can read and write, saved in the node's flash;
  - **actions**: things a host can ask it to do once ("zero the encoders");
  - **events**: numbered problems or happenings it reports.
- Every node also has everything in `protocol.yaml`: heartbeat, E-stop, arm/disarm, ID
  assignment, the parameter service, firmware updates, and common parameters such as
  `heartbeat_hz`.

## Identifiers

Frames use **29-bit extended IDs**, split like this (from `protocol.yaml`, `id_layout`):

```
bit  28   26 25          18 17               8 7           0
    [priority][    dst     ][      port       ][    src     ]
      3 bits     8 bits          10 bits          8 bits
```

| Part | Meaning |
|---|---|
| `priority` | 0 (most urgent) to 7. Lower wins arbitration. E-stop and SYNC are 0. |
| `dst` | Which node it's for. 0 = everyone. |
| `port` | Which message. 0–63 belong to the protocol; 64–1023 to the sender's (or, for commands, the receiver's) node type. |
| `src` | Who sent it. |

- Node IDs 1–254 are given out at run time. Hosts have IDs too, from their config. 255 means
  "no ID yet" (a new board asking for one).
- Because `src` is in the ID, no two senders ever use the same ID, which CAN requires.
- `dst` is in the top 16 bits so the ESP32's hardware filter can accept "for me" and
  "for everyone" without the CPU seeing anything else.
- Port numbers are per node type: port 64 on a motor controller and port 64 on a sensor board
  are different messages. Hosts know which is which from each node's heartbeat.
- The widths are settings in `protocol.yaml`, not numbers in code. Change them (they must add
  up to 29, priority first) and every generator follows.

`anv-schema id 0x14000610` splits an ID for you.

## Adding a node type

1. Create `schema/types/<name>.yaml` (copy `types/esp32_bench.yaml` or an example).
2. Pick a `type_id` nobody else uses (`anv-schema show` lists them).
3. Describe messages, parameters, actions and events (below).
4. Run `anv-schema check`, then `anv-schema gen`, and commit the YAML and `generated/`.
   CI fails if `generated/` is out of date.
5. In the firmware, include `anv_<name>.h` and compile `anv_<name>.c`.

## File format

### The type

```yaml
name: motor_controller        # lower_case; becomes the C prefix anv_motor_controller_
type_id: 100                  # 2..65535, unique (0 = the protocol, 1 = hosts)
version: 1                    # bump when you change it incompatibly
description: Two-channel motor controller.
include: [power_monitor.yaml] # optional, see "Reusing pieces"
enums: {...}
messages: [...]
params: [...]
actions: [...]
events: [...]
```

### Messages

```yaml
messages:
  - name: setpoint
    port: 64                  # 64..1023, unique within the type
    kind: command             # stream | event | command | frame
    sender: leader            # node | host | leader | any (defaults: commands leader, else node)
    priority: 1               # optional; defaults: command 1, stream 2 (3 if under 20 Hz),
                              #   event 3, frame 6
    watchdog_ms: 100          # commands: outputs stop if none arrives this long (0 = no watchdog)
    apply: sync               # commands: now (default) or sync (held until the next SYNC)
    description: What each channel should do
    fields:
      - {name: mode, type: u8, enum: mode}
      - {name: target, type: i16, count: 2, scale: 0.01, unit: rad/s}
```

| `kind` | Meaning |
|---|---|
| `stream` | Sent periodically by a node. Needs `rate`. A host can change the rate at run time. |
| `event` | Sent when something happens. |
| `command` | Sent by a host to one node (`dst`). Can have a watchdog and be applied at SYNC. |
| `frame` | Raw 8 bytes with no fields (the ISO-TP transfer channel). |

Rates for streams, in Hz: `rate: 10`, or `rate: {default: 10, min: 0, max: 50}`. `min`/`max`
bound what a host may set; `default: 0` means off until someone turns it on (good for debug
streams).

### Fields

Fields are packed one after another from bit 0, little-endian (the DBC "Intel" layout). A
frame holds 64 bits.

| Key | Meaning |
|---|---|
| `name` | lower_case identifier |
| `type` | `bool` (1 bit), `uN` (N = 1..64), `iN` (N = 2..64, two's complement), `f32` |
| `count` | an array: the type repeated `count` times back to back |
| `scale`, `offset` | physical = raw × scale + offset |
| `unit` | for people and tools |
| `min`, `max` | physical limits; encoding refuses values outside them |
| `enum` | name of an enum (this type's or the protocol's), or an inline `{label: number}` map |
| `const` | the field always carries this raw value (checked on decode) |
| `start_bit` | place the field yourself instead of right after the previous one |
| `description` | for people |

Use small types freely: `u4` for a 0–15 value, `bool` for a flag. Name unused bits
`reserved` so tools skip them.

### Parameters

```yaml
params:
  - {index: 0x0000, name: kp, type: f32, count: 2, min: 0, max: 1000, default: 10,
     disarmed_only: true, group: pid, description: Velocity P gain per channel}
```

| Key | Meaning |
|---|---|
| `index` | 0x0000..0xEFFF, unique (0xF000 and up are the protocol's common parameters) |
| `type` | `bool u8 i8 u16 i16 u32 i32 f32` (anything that fits the 32-bit service value) |
| `count` | makes `kp_0`, `kp_1`, ... at consecutive indices |
| `default`, `min`, `max`, `unit`, `enum` | as you'd expect |
| `access` | `rw` (default) or `ro` (the node reports it, nobody sets it) |
| `persist` | saved by SAVE (default: true for rw) |
| `disarmed_only` | SET is refused while the node is armed |
| `reboot_required` | takes effect after a reset |
| `group` | for user interfaces: parameters with the same group show together |

### Actions

```yaml
actions:
  - {index: 1, name: zero_counts, description: Reset the encoder counts}
  - {index: 2, name: jog, arg: {type: f32, min: -10, max: 10, unit: rad/s}, disarmed_only: true}
  - {index: 3, name: autotune, arg: u8}
```

An action takes at most one argument (same types as parameters).

### Events

```yaml
events:
  - {code: 0x4001, name: overcurrent, severity: error, description: "Over max_current (detail = channel)"}
```

Codes: 0x0001–0x3FFF protocol, **0x4000–0x7FFF node types**, 0x8000–0xFFFF raised by hosts.
Severity: `info`, `warning`, `error`, `critical`. An event frame also carries a 16-bit
`detail` whose meaning the description gives.

### Enums

```yaml
enums:
  mode: [coast, brake, velocity, duty]     # coast = 0, brake = 1, ...
  gear: {low: 1, high: 2}                  # or explicit numbers
```

Types can also use the protocol's enums (`node_state`, `severity`, ...).

### Reusing pieces: `include`

A fragment (`fragments/*.yaml`) has messages, params, actions, events and enums, but no name
or type_id. A type pulls it in:

```yaml
include:
  - power_monitor.yaml                                  # as is
  - {file: power_monitor.yaml, prefix: aux_, port_offset: 1, index_offset: 0x10,
     event_offset: 0x10}                                # a second copy, renamed and moved
```

`prefix` renames every item; the offsets move ports, parameter indices, action indices and
event codes, so the same fragment can appear twice without clashes.

### One YAML trap

Inside `{...}`, a comma ends the value. `description: big, very big` becomes a description of
"big" and a stray key "very big". The loader reports stray keys, so you'll see it, but the fix
is to quote: `description: "big, very big"`.

## Using the generated code

### C (firmware)

```c
#include "anv_motor_controller.h"

anv_motor_controller_feedback_t fb = {0};
fb.speed[0] = anv_motor_controller_feedback_speed_from_phys(12.5f);   /* rad/s -> raw */
uint8_t data[8];
uint8_t len = anv_motor_controller_feedback_pack(data, &fb);
uint32_t id = anv_id_make(ANV_MOTOR_CONTROLLER_FEEDBACK_PRIORITY, ANV_NODE_BROADCAST,
                          ANV_MOTOR_CONTROLLER_FEEDBACK_PORT, my_node_id);
```

- Structs hold **raw** values; `*_to_phys` / `*_from_phys` convert (rounding and clamping).
  They use `float` (the ESP32's FPU is single precision); define `ANV_SCHEMA_PHYS_DOUBLE` for
  `double`.
- `anv_<type>_type` describes the whole type (messages, parameters, actions, events). The node
  library works from this table, so it never needs to know which type it's running.
- Define `ANV_SCHEMA_NO_NAMES` to drop name strings from the tables and save flash.
- Compile only the `.c` files for the types your board uses.

### Python (hosts, tools, tests)

```python
import anv_schema
s = anv_schema.load()                              # or anv_schema.load_bundle("schema.json")
t = s.types["motor_controller"]
m = t.message("setpoint")
data = anv_schema.encode(m, {"mode": "velocity", "target": [1.5, -2.0]})
can_id = anv_schema.make_id(s, m, src=1, dst=20)
anv_schema.decode(m, data)          # {'mode': 'velocity', 'target': [1.5, -2.0], ...}
frame = anv_schema.decode_frame(s, can_id, data, t)   # splits the ID too
```

Values are physical (scaled, enum labels) by default; `raw=True` gives the wire integers.

### On the command line

```sh
anv-schema encode motor_controller setpoint mode=velocity "target=[1.5,-2]" --src 1 --dst 20
# 04504001#02960038FF0000   (cansend format: cansend can0 04504001#02960038FF0000)
anv-schema decode 0x04504001 02960038FF0000 --type motor_controller
anv-schema dbc schema/buses/bench.yaml -o bench.dbc   # for SavvyCAN / cantools
```

## Compatibility: the wire hash

Every type has a 32-bit **wire hash** over everything that affects the bytes on the bus (ports,
layouts, scaling, parameter and action indices and types, event codes, type ID, version). It
ignores descriptions, units, defaults and limits. A node sends the low 16 bits in its heartbeat;
a host compares them with its own schema and reports `schema_mismatch` rather than decoding
garbage. If you change a type, regenerate and reflash; the hash tells you who is out of date.
