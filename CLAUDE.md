# Anveshak rover CAN system — rules for every Claude session

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

## Fixed design decisions (don't change these)

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
defs/                      shared definitions (owned by P0; changes need a note in the PR)
  rover.dbc                all CAN messages
  errors.yaml              error-code registry
  params/<node_type>.yaml  parameter tables
  profiles/                dashboard profiles (later)
  codegen/                 scripts: DBC -> C, errors -> C + Python, params -> C + Python
firmware/                  person A
  core/                    anveshak_can_core (C11, no malloc, no RTOS dependency in core)
  drivers/                 bxcan/, fdcan/, twai/, loopback/ (for tests)
  nodes/drive_stm32/       drive node application
  nodes/sensor_esp32s3/    ESP32-S3 node template (ESP-IDF component + app)
  bootloader_stm32/        CAN bootloader (later)
  tests/                   host-side unit tests (run on Linux with the loopback driver)
host/                      person B
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

## Testing

- Everything must be testable without rover hardware.
- Firmware core: build for Linux and test with the `loopback` driver (an in-process fake bus).
- Host side: use `python-can` with its `virtual` interface, or an in-process fake bus. Use
  Linux `vcan` only if `sudo ip link add dev vcan0 type vcan` works in this VM.
- STM32 code: compile with `arm-none-eabi-gcc` (install with apt if missing). It can't be run
  here; compile-only is fine.
- ROS 2: CI builds on `ros:humble` and `ros:jazzy` containers.

## Code style

- C: C11, `snake_case`, fixed-width ints, no dynamic allocation in firmware.
- C++: C++17, no exceptions across the gateway's public API.
- Python: 3.10+, type hints, `ruff` clean.
- Keep files small and focused; comment the *why*, not the *what*.
