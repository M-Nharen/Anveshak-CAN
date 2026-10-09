# Gateway core (`host/gateway`)

`anv_can_gateway` is the one program that owns `can0` on a host. It decodes every frame, tracks
every node, runs the leader election, and, while it is master, runs the 20 ms control cycle.
It is a plain C++17 library (`anv_gateway`) plus a small executable. There is no ROS dependency,
so the same code runs on the Orin and the RPi under any ROS distro. The ROS 2 bridge (B2) and the
dashboard (B3) sit on top of the C++ API below.

The spec is `docs/architecture.md` ("Hosts: choosing which one leads", "Host software").

## Build and test

```
cmake -S host/gateway -B build-gateway -DANV_WERROR=ON
cmake --build build-gateway -j
ctest --test-dir build-gateway --output-on-failure
```

Needs only CMake, a C++17 compiler and pthreads. Tests are plain executables (no test framework).

| Test | What it covers |
| --- | --- |
| `test_decoder` | encoder/decoder round trips; every DBC message ID decodes (117 IDs) |
| `test_config` | the flat YAML config |
| `test_leadership` | every election rule with a fake clock |
| `test_registry` | alive/missing at 300 ms, reboot by uptime, rates, stored values |
| `test_gateway` | two gateways on the in-process fake bus, real threads: no request means no leader, handover with rising terms, leader killed (LEADER_LOST at ~250 ms, optional auto-takeover), stale targets become zero speed, E-stop/ARM rules |
| `vcan_smoke` | the real executable plus the Python simulator on `vcan0`; skips itself (exit 77) if `vcan`, python-can or cantools is missing |

## Run

```
anv_can_gateway --config host/gateway/config/gateway.example.yaml
```

Config keys (flat `key: value` YAML, `#` comments): `interface`, `host_id` (1 Orin, 2 RPi),
`tie_break`, `auto_takeover` (default `false`), `rt_priority` (SCHED_FIFO for the CAN threads,
default 50, 0 = off), `cpu_affinity` (pin the CAN threads, default off), `bridge_socket`
(Unix socket path for the dashboard backend, default off; also `--bridge-socket`). A failure to get real-time
priority is logged, not fatal.

It starts as standby and prints a status block every second. Commands on stdin:

```
lead 1|2      ask host 1 (Orin) or 2 (RPi) to lead         arm | disarm
drive A B     all drive nodes, A and B in rad/s, until stop   stop
estop         E-stop                                          release   (master only)
status        print now                                       quit
```

Against the simulator: `python host/sim/cli.py --interface socketcan --channel vcan0`, then run
the gateway on `vcan0` and type `lead 1`, `arm`, `drive 2 2`.

## C++ API (`anv_gateway/gateway.hpp`)

```cpp
Gateway gw(config, std::make_unique<...>(SocketCanBus or FakeBus port));
gw.start();
gw.request_lead(2);                       // LEAD_REQUEST on the bus (and applied locally)
gw.set_targets({...}); gw.set_wheel_target(3, {...});   // held 100 ms, then zero speed
gw.arm(); gw.disarm();                    // master only
gw.estop(); gw.release_estop();           // E-stop from any role; release is master only
GatewayState s = gw.state();              // role, term, nodes, rates, bus stats, late cycles
int id = gw.subscribe([](const GatewayEvent& e) { ... });
```

All methods are thread-safe and return a `CmdResult` instead of throwing. Event callbacks run on
the gateway's own threads: keep them short.

## Layout

| File | Job |
| --- | --- |
| `can_bus.hpp`, `socketcan_bus.*`, `fake_bus.*` | CAN I/O behind `ICanBus`: SocketCAN (kernel timestamps, kernel ID filters, error frames) or an in-process bus for tests |
| `decoder.*`, `encoder.*`, `messages.hpp` | C++ wrappers over the generated code in `firmware/generated`; no hand-written byte packing |
| `node_registry.*` | per-node health, reboot detection, latest feedback, measured vs expected rates |
| `leadership.*` | the election as pure logic, driven by frames and a clock |
| `gateway.*` | RX thread, control thread (absolute-deadline `clock_nanosleep`, 10 ms tick: cycle every 2nd tick, HOST_HB every 5th), local API |
| `config.*`, `status.*`, `src/main.cpp` | config file, text status, executable |

## Decisions worth knowing

- **Arming belongs to one (role, term).** Claiming or stepping down clears the gateway's own
  "armed" flag, mirroring nodes disarming on a term change; the new master must ARM again.
- **A claim is announced at once.** After a claim or step-down the next control tick sends
  HOST_HB immediately (well inside the 50 ms rule) instead of waiting for the 20 Hz slot.
- **Lead requests.** A request is a duplicate only if the same sender repeats the same counter
  within 500 ms; the same counter later counts as new (a restarted tool resets its counter).
  A request naming the other host needs no action: that host claims with a higher term and we step
  down when we hear it.
- **E-stop is fail-safe.** Any frame on ID 0x000 latches it, whatever its payload. While latched
  the master keeps sending commands, all at zero speed, so node watchdogs stay fed.
- **Zero speed, not silence.** Stale targets (older than 100 ms, or never set) are sent as
  velocity-mode zero commands every cycle.
- **Not done here:** refusing to ARM a node with an older protocol version (`defs/` has no
  "expected protocol version" to compare against yet) and the rolling frame log from the "Host
  software" table (B1's build list does not include it). Gateway link, debug levels, the error log
  and the flight recorder are B4; ROS 2 is B2; the dashboard is B3.

## Dashboard bridge (`anv_gateway/bridge.hpp`)

With `bridge_socket` set, the executable serves a Unix socket (mode 0660) carrying one JSON object
per line, one reply line per request: `state`, `set_targets` (6 wheels), `arm`, `disarm`, `estop`,
`release_estop`, `request_lead`, `ping`. It only wraps the public `Gateway` API, so master-only
rules, the E-stop latch and the 100 ms target hold stay in the gateway. The dashboard backend
(`dashboard/backend`) is its client; the format is documented at the top of `bridge.hpp`.
