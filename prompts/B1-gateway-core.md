# B1 — Gateway core (C++, no ROS)

**Who:** person B. **Needs:** P0 merged. **Folders:** `host/gateway/`, the gateway job in
`.github/workflows/ci.yml`.

## Goal

The program that owns `can0` on each host: talks to the bus, decodes every frame, tracks every
node, chooses the leader as the operator asks, and, when leading, runs the 20 ms control cycle.
Built as a plain C++ library plus a small executable, with no ROS dependency, so it works on any
host and any ROS distro.

## Read first

`docs/architecture.md` sections: "Hosts: choosing which one leads" (all of it), "Host software"
(the "Inside the gateway" table and "Running the 20 ms cycle reliably"), and the HOST_HB,
LEAD_REQUEST, SYNC, NMT, DRIVE_CMD and NODE_HB layouts in "Message catalog".

## Build

1. **CAN I/O** with SocketCAN (raw socket, kernel timestamps, error frames enabled), behind an
   interface so tests can use an in-process fake bus.
2. **Decoder** using the generated C code from `firmware/generated/` (wrap it in C++) or a C++
   generator; every DBC message decoded into typed structs.
3. **Node registry**: alive/missing (300 ms), state, firmware build, uptime (detects reboots),
   TEC/REC, measured vs expected rate per ID.
4. **Leadership** exactly as in the doc: start as standby; claim only on a LEAD_REQUEST naming
   this host (or a local request); term = highest seen + 1; higher term wins; tie-break number;
   step down on a higher term; LEADER_LOST after 250 ms without a master heartbeat; optional
   `auto_takeover` (default false); HOST_HB at 20 Hz.
5. **Cycle scheduler** (only when master): `clock_nanosleep` absolute 20 ms deadlines; SYNC, then
   the six DRIVE_CMDs from the latest targets; zero speed if targets are older than 100 ms; count
   late cycles.
6. **Local API** for later layers (ROS 2, dashboard): set targets, arm/disarm, E-stop, request
   lead, read state, subscribe to events. Thread-safe.
7. **Executable** `anv_can_gateway` with a YAML config (`host_id`, `tie_break`, `auto_takeover`,
   interface name) and a simple text status output.

## Tests

Using the in-process fake bus (and the P0 simulator if `vcan` is available):

- With two gateways and no request, nobody leads and no commands are sent.
- A LEAD_REQUEST for host 2 makes it lead; then a request for host 1 hands over; the term rises
  each time and only one host sends commands at any moment.
- Killing the leader: LEADER_LOST raised at about 250 ms; no automatic takeover by default; with
  `auto_takeover` on, the standby claims.
- Stale targets produce zero-speed commands after 100 ms.
- The node registry flags a missing node within 300 ms and a rebooted one when its uptime drops.

## Done when

- `cmake --build` and `ctest` pass on Ubuntu 22.04 and 24.04 (CI matrix).
- Running `anv_can_gateway` against the simulator on `vcan0` (if available) shows all simulated
  nodes and drives them after a lead request and ARM.

## Out of scope

ROS 2 (B2), dashboard (B3), gateway link, debug levels, error log (B4).
