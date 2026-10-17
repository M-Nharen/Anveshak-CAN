# L3 — anv_node: the generic node runtime (ESP32, C)

Branch: `lib/node` from `next` (after L1 is merged). PR into `next`.

## Read first
- `schema/README.md`, `schema/protocol.yaml`, the example node types
- `firmware/core` (existing arm/disarm, E-stop, watchdog, heartbeat, params)
- The bench sketch's ID assignment and `user_app.cpp` pattern

## Build
- `anv_node` on top of `anv_link` + `anv_proto`. A node declares, from generated schema code:
  settings (typed, limits, units, saved to flash, "only while disarmed" flag),
  signals grouped into streams with run-time-changeable rates, commands with a watchdog,
  and one-shot actions.
- Built-in behaviour every node gets: heartbeat announcing node type + schema version,
  automatic ID assignment (and a way to save a fixed ID), arm/disarm, E-stop, leader tracking,
  disarm on watchdog timeout or leader change, outputs off by default. Safety can't be disabled.
- A user hook API: `setup` / `loop` callbacks, setting-changed callbacks, raw-frame escape hatch.
  No `delay()` in the runtime.
- Port the bench sketch to use this library.

## Done when
- Linux tests (loopback) cover: ID assignment, settings read/write/save, stream rate change,
  watchdog disarm, E-stop, leader change.
- The ported bench sketch builds for ESP32.

## Don't
- No firmware flashing or error events yet — that's L5.
