# P0 — Shared definitions, code generation, simulator, CI skeleton

**Who:** person A, before anything else. **Folders:** `defs/`, `host/sim/`, `.github/workflows/`.

## Goal

Create the single source of truth for the CAN protocol and the tools that turn it into code,
plus a simulator that behaves like the rover's nodes, so both people can start building in
parallel.

## Read first

`docs/architecture.md` sections: "Identifier scheme", "Message catalog", "Debug levels" (the
DEBUG_LEVEL and DRIVE_DEBUG tables only), and the error-code table in "Error reporting and
diagnostics".

## Build

1. `defs/rover.dbc` containing **every** message in the "Message catalog" overview table,
   including DEBUG_LEVEL, LEAD_REQUEST, DRIVE_DEBUG_A/B and NODE_EVENT. Use the drive node 1 IDs
   for drive messages and add the other five drive nodes (IDs 5–9) by the +8 rule. Correct
   signal names, scaling, units, signedness and little-endian byte order. Put the protocol
   version in a DBC attribute `ProtocolVersion` (start at 1).
2. `defs/errors.yaml`: every code from the error table (code, name, severity, description,
   likely cause).
3. `defs/params/drive.yaml`: parameter table for drive nodes (index, name, type, min, max,
   default, unit, "changeable only while disarmed" flag). Include PID gains, current limit, max
   speed, encoder counts/rev, brake-or-coast, all message rates (`rate_fb_hz`, `rate_telem_hz`,
   `rate_hb_hz`), `debug_default`.
4. `defs/codegen/`: a Python script (`generate.py`) that produces:
   - C pack/unpack code from the DBC (use `cantools generate_c_source`) into
     `firmware/generated/`
   - a C header of error codes and a Python module of error codes (with text)
   - a C parameter table and a Python parameter table
   - a C header and a Python module with the ID helpers and node-ID constants
   Generated files are committed. The script is idempotent.
5. `defs/codegen/check.py`: a validator. Fail if two messages share an ID, any ID breaks the
   class/node/sub rule, a message has no sender, or an error code is duplicated.
6. `host/sim/`: a Python simulator (`python-can`, `cantools`) that pretends to be 6 drive nodes
   and the FPGA. It answers SYNC with feedback, follows DRIVE_CMD only after ARM from the current
   master, disarms on a 100 ms command timeout and on term changes, sends heartbeats and 10 Hz
   telemetry, and supports fault injection from the command line (`--drop-node 6`,
   `--overcurrent 4`). Default interface: `python-can` `virtual` bus; `--interface socketcan
   --channel vcan0` also works.
7. `.github/workflows/ci.yml`: run the codegen check, then confirm the generated files are up to
   date (regenerate and `git diff --exit-code`), then run the simulator's tests. Leave clear
   placeholders (commented jobs) for firmware, gateway (Humble + Jazzy) and dashboard jobs that
   later steps will fill in.

## Done when

- `python defs/codegen/generate.py && python defs/codegen/check.py` pass.
- `pytest host/sim` passes, including a test that arms a simulated node, sends commands, then
  stops sending and checks the node disarms within 100 ms (simulated time is fine).
- CI is green on the PR.

## Out of scope

Firmware, gateway and dashboard code.
