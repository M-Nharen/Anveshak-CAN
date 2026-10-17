# anv_can — host CAN library (C++17, with Python bindings)

The host side of `anv_link` + `anv_proto`, for the Raspberry Pi and the Jetson. Everything about
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
| `python/` | pybind11 module `anv_can` and the `anv-can` tool |

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

Not here (L4): discovery, leader election, parameter clients.
