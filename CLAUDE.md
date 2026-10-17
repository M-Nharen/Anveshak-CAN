# Anveshak rover CAN system — rules for every Claude session

**Every node is an ESP32 (WROOM or S3).** STM32 support was removed on 2026-10-18; the STM32
sections of `docs/architecture.md` are historical. Don't add STM32 code back.

**New code is built on the libraries (branch `next`, see README.md).** Everything on the bus is
described in `schema/` (format: `schema/README.md`) and code is generated from it with
`anv-schema gen`. Never write message layouts, ports, parameter indices or event codes by hand,
and never hardcode how many nodes there are or what IDs they have: types are data, IDs are
assigned at run time. `defs/` is the old (11-bit) system and goes away once the apps move over.

The full design is in `docs/architecture.md`. It is the spec. Read only the sections your
prompt names; don't read the whole file unless the prompt says so.

## How to work (these rules keep cost down)

- Do **only** the task in the prompt file you were given (`prompts/<id>.md`). Don't refactor,
  restyle or "improve" other folders.
- Stay inside the folders your prompt lists. If you must touch anything else (especially
  `defs/`), keep the change minimal and explain it in the PR description.
- Don't explore the repo broadly. Read the files you need, then build.
- Work in small steps: write a piece, build it, test it, continue.
- Stop when the prompt's "Done when" list is satisfied. Don't add extra features.
- At the end: make sure everything builds and all tests pass, commit on your branch with a
  clear message, and write a short PR description: what was built, how to test it, what is
  left or stubbed, and any change you made outside your folders.
- If a tool you need can't be installed in this cloud VM (ROS 2, ESP-IDF, `vcan`), write the
  code anyway, make it compile where possible, and add or extend the GitHub Actions workflow
  so CI builds and tests it. Note this in the PR.

## Library tasks (`prompts/v2/`)

New library work follows `prompts/v2/README.md` and the **Schema rules** below: 29-bit
extended IDs, everything generated from `schema/`. Bitrate, cycle rate and watchdog timeout
are settings with defaults (500 kbps, 50 Hz, 100 ms), not constants. The safety rules still
apply: outputs off by default, disarm on command timeout, on leader change and on fault;
E-stop always works.

## Old system (`defs/` and the current apps only)

These describe the existing 11-bit apps (gateway, dashboard, ROS, simulator, bench sketch).
Keep them working, but don't use these for new library code.

- Classic CAN 2.0A, 11-bit IDs, 8-byte payloads, **500 kbps**, sample point ~80%.
- Control cycle **50 Hz** (SYNC every 20 ms). Node command watchdog **100 ms**.
- ID = (class << 8) | (node << 3) | sub. Classes, node IDs and every message layout are in
  `docs/architecture.md` sections "Identifier scheme" and "Message catalog".
- All message layouts come from `defs/rover.dbc`. Error codes from `defs/errors.yaml`.
  Parameters from `defs/params/*.yaml`. Never hand-write byte packing: use generated code.
- Little-endian, scaled integers.
- Only the leading host ("master") sends commands, SYNC, NMT, ESTOP_RELEASE, DEBUG_LEVEL.
  ESTOP (0x000, fixed payload 0xE5) may be sent by anyone.
- Nodes: outputs off by default; disarm on command timeout, on master/term change, on fault.
- Hosts may run ROS 2 **Humble or Jazzy**, independently. Hosts never talk to each other over
  ROS: only CAN and the UDP gateway link.

## Repository layout

```
schema/                    THE definitions: protocol.yaml, types/, fragments/, generated/
defs/                      old 11-bit definitions, still used by the current apps
  rover.dbc                all CAN messages
  errors.yaml              error-code registry
  params/<node_type>.yaml  parameter tables
  profiles/                dashboard profiles (later)
  codegen/                 scripts: DBC -> C, errors -> C + Python, params -> C + Python
firmware/                  person A
  core/                    anveshak_can_core (C11, no malloc, no RTOS dependency in core)
  drivers/loopback/        in-process fake bus (for tests)
  nodes/bench_esp32_arduino/  ESP32 (WROOM / S3) node sketch, Arduino
  tests/                   host-side unit tests (run on Linux with the loopback driver)
host/                      person B
  python/anv_schema/       schema loader, codec, generators, `anv-schema` CLI
  gateway/                 C++17 core library, no ROS dependency
  ros2/anveshak_can_ros/   ROS 2 node wrapping the gateway (Humble + Jazzy)
  ros2/anveshak_can_msgs/  ROS 2 messages and services
  sim/                     Python node simulator
  tools/                   anv-flash and other Python tools
dashboard/                 person B
  backend/                 FastAPI + WebSocket
  frontend/                Svelte + uPlot
.github/workflows/         CI
```

## Schema rules

- 29-bit IDs: priority(3) | dst(8) | port(10) | src(8), from `schema/protocol.yaml`.
- Ports 0-63 are the protocol's; node types use 64-1023. Parameter indices 0xF000+ are the
  protocol's common parameters. Event codes 0x4000-0x7FFF are per node type.
- After changing anything in `schema/`: `anv-schema check && anv-schema gen`, commit
  `schema/generated/`. CI fails if it's stale.

## Testing

- Everything must be testable without rover hardware.
- Firmware core: build for Linux and test with the `loopback` driver (an in-process fake bus).
- Host side: use `python-can` with its `virtual` interface, or an in-process fake bus. Use
  Linux `vcan` only if `sudo ip link add dev vcan0 type vcan` works in this VM.
- ESP32 code: CI compiles it with `arduino-cli`; it can't be run here.
- ROS 2: CI builds on `ros:humble` and `ros:jazzy` containers.

## Code style

- C: C11, `snake_case`, fixed-width ints, no dynamic allocation in firmware.
- C++: C++17, no exceptions across the gateway's public API.
- Python: 3.10+, type hints, `ruff` clean.
- Keep files small and focused; comment the *why*, not the *what*.
