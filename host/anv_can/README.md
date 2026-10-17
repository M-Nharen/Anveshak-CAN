# anv_can — host CAN library (C++17, with Python bindings)

The host side of `anv_link` + `anv_proto`, plus `anv_host` (discovery, IDs, leadership and the
node clients), for the Raspberry Pi and the Jetson. Everything about
the bus comes from `schema/generated/schema.json` at run time (see `schema/README.md`): ID
widths, ports, layouts, enums. Nothing here knows how many nodes there are or what types.

| Part | What it is |
|---|---|
| `anv/link.hpp` | `Link`: send, receive, filters, counters. `Frame`, `Filter`, cansend text format |
| `anv/socketcan_link.hpp` | SocketCAN `Link` (real CAN and vcan): kernel filters, timestamps, drop/bus-off counters |
| `anv/test_bus.hpp` | In-process bus for tests: filters, queue limits, a tap to lose frames on purpose |
| `anv/schema.hpp` | Loads `schema.json`; `IdLayout` packs/splits 29-bit IDs |
| `anv/codec.hpp` | Encode/decode messages by name, physical or raw; `decode_frame` |
| `anv/isotp.hpp` | ISO-TP on the `transfer` port: non-blocking sender/receiver, blocking `IsoTpChannel` |
| `anv/host/host.hpp` | `Host`: a brain on the bus (below) |
| `anv/host/election.hpp` | Leader election rules, pure logic with a fake clock |
| `anv/host/registry.hpp` | Node registry: discovery from heartbeats, type lookup, schema check, online/offline |
| `anv/host/allocator.hpp` | ID assignment server: MAC -> ID table kept in a text file |
| `python/` | pybind11 module `anv_can` and the `anv-can` and `anv-host` tools |

Frames are byte-identical to the ESP32 side: `tests/test_proto.cpp` compares against the
generated C, and `python/tests/test_cross_check.py` compares every message with random vectors
against `anv_schema`'s codec (which `anv_schema`'s own tests compare with the generated C).

ISO-TP: classic 8-byte frames padded with 0xAA, like the bench node's OTA code. Up to 4095 bytes
use the normal first frame; longer payloads the ISO 15765-2:2016 escape (32-bit length). A
missing or out-of-order consecutive frame aborts the transfer (`LostFrame`); the sender
retries. Bitrate is set on the interface, not here:
`ip link set can0 type can bitrate 500000 restart-ms 100` (restart-ms recovers from bus-off).

## Build and test

```sh
cmake -S host/anv_can -B build-anv -DANV_WERROR=ON
cmake --build build-anv -j && ctest --test-dir build-anv --output-on-failure

pip install ./host/anv_can ./host/python/anv_schema pytest   # bindings + the cross-check's reference
python -m pytest host/anv_can/python/tests
```

CMake links `anv_proto` (which pulls in `anv_link`). Built from the tree, the library finds the
schema itself; elsewhere set `ANV_SCHEMA_JSON`. The Python wheel carries a copy of the schema it
was built with.

## Python

```python
import anv_can
s = anv_can.load_schema()
m = s.type("motor_controller").message("setpoint")
data = anv_can.encode(m, {"mode": "velocity", "target": [1.5, -2.0]})
link = anv_can.open_socketcan("vcan0")              # or anv_can.TestBus().connect()
link.send(anv_can.Frame(anv_can.make_id(s, m, src=1, dst=20), data))
f = link.recv(100)                                   # Frame or None
anv_can.decode_frame(s, f.id, f.data, s.type("motor_controller"))
```

## `anv-can`

```sh
anv-can dump vcan0                                   # decode live traffic
anv-can dump can0 --type 20=motor_controller         # don't wait for node 20's heartbeat
anv-can send vcan0 protocol nmt command=arm term=3 --src 1 --dst 20
anv-can send vcan0 motor_controller setpoint mode=velocity "target=[1.5,-2]" --dst 20
anv-can send vcan0 04504001#02960038FF0000           # raw, cansend format
```

`dump` learns each node's type from its heartbeat; until then a node-type port shows as unknown.

## anv_host

`Host` joins the bus with an ID from its config and runs one background thread. It knows
nothing about the rover: nodes are discovered from their heartbeats, and each node's type is
looked up in the schema by `type_id`.

- **Registry.** Every node with its type, state, uptime, reboots, measured heartbeat rate and
  MAC. A node whose heartbeat hash doesn't match our schema's type is listed as
  `schema_mismatch` and nothing is decoded from or asked of it. Offline after
  `node_timeout_ms` (or 3.5 heartbeat periods, if longer).
- **IDs.** Answers `id_request` with an ID from `id_first..id_last` (default: the schema's
  range) and keeps MAC -> ID in `id_table_path`, so a board always gets its ID back.
  `allocator`: `off`, `always`, or `auto` (default): answer while leading, or while nobody leads
  and this is the lowest-numbered host that could lead, so two hosts never answer.
- **Leadership.** docs/architecture.md "Hosts: choosing which one leads", for any hosts with
  any IDs: start standby, claim only when a `lead_request` names you (`request_lead()`) or with
  `auto_takeover`, higher term wins, then lower `tie_break`, then lower ID. `observer` hosts
  never lead. While leading: SYNC at `sync_hz`, refreshed commands.
- **Clients.** `get`/`set`/`save`/`restore_defaults` (node type's parameters and the protocol's,
  by name or index), `get_rate`/`set_rate` (Hz), `action`, `info`; checked against the schema
  (limits, read-only, types) before anything is sent. Leader only: `send_command`,
  `set_command` (kept refreshed at `command_hz` until `stop_command`; dropped on losing the lead
  or E-stop), `arm`/`disarm`/`nmt`, `estop_release`. Anyone: `estop`.
- **Callbacks** (set before `start()`): node appeared/lost/rebooted/state, telemetry (decoded
  streams and events of a node's own type), node events, setting changed (any host's SET),
  role/leader changed, leader lost, two leaders, ID assigned, E-stop.

On the wire: a service value is the C `anv_value_t` union (unsigned as is, signed
sign-extended, f32 as its bits), rates are mHz, and a node answers a SET with the value it now
holds.

```python
import anv_can
s = anv_can.load_schema()
host = anv_can.Host(anv_can.open_socketcan("can0"), s, host_id=1, id_table="/var/lib/anv/ids.txt")
host.set_callbacks(node_appeared=lambda n, returned: print("hello", n),
                   telemetry=lambda node, msg, values, ts: print(node, msg, values))
host.start()
host.request_lead()                       # the operator's choice
host.set(20, "heartbeat_hz", 20)
host.arm(20)
host.set_command(20, "setpoint", {"mode": "velocity", "target": [1.0, 1.0]})   # 50 Hz until stopped
```

Errors raise `anv_can.HostError` (`"not_leader"`, `"timeout: ..."`, `"out_of_range: ..."`).

## `anv-host`

```sh
anv-host list vcan0                          # nodes and other hosts
anv-host get vcan0 20 [PARAM ...]            # default: every setting
anv-host set vcan0 20 kp_0=12.5 --save
anv-host rate vcan0 20 [STREAM [HZ]]
anv-host action vcan0 20 blink 5
anv-host arm vcan0 20 --take-lead            # holds the lead until Ctrl-C (or --hold S)
anv-host command vcan0 20 setpoint mode=velocity "target=[1,1]" --take-lead --arm --duration 2
anv-host disarm vcan0 all --take-lead
anv-host estop vcan0 [--release --take-lead]
anv-host lead vcan0 2                        # ask host 2 to lead
```

The tool is a host with ID `--host-id` (default `$ANV_HOST_ID`, else 250) that never hands out
IDs. Taking the lead disarms every node (new term); when the tool exits, nodes it armed disarm.
