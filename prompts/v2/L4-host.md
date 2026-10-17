# L4 — anv_host: discovery and control (C++ with Python bindings)

Branch: `lib/host` from `next` (after L2 is merged). PR into `next`.

## Read first
- `schema/README.md`, `schema/protocol.yaml`
- `host/gateway/` (existing leader election and node tracking)

## Build
- `anv_host` on top of the host `anv_link` + `anv_proto`:
  - Node registry: discover nodes from heartbeats, look up each node's type in the schema,
    check the schema version, track online/offline.
  - ID assignment server (the host side of automatic IDs).
  - Leader election between brains, with operator choice. Nothing assumes Orin or Pi specifically.
  - Clients: read/write/save settings, set stream rates, send commands (with the 50 Hz-style
    refresh as a setting), call actions, arm/disarm, E-stop.
  - Callbacks for node appeared/lost, telemetry, and setting changes.
- Python bindings for all of the above.
- A CLI: `anv-host list`, `get`, `set`, `rate`, `arm`, `estop`.
- Nothing hardcodes rover layout (no wheel counts, no node lists).

## Done when
- Tests pass against the test bus with fake nodes written in the test itself.
- The CLI works on vcan0.

## Don't
- No dashboard or ROS work. No flashing — that's L5.
