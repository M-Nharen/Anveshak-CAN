# anv_node

The generic node runtime for the v2 CAN system, on top of `anv_link` and `anv_proto`. A node is
described entirely by its generated type table (`anv_<type>_type` from `schema/generated/c`), so
the same runtime runs every node type. The application only does its job: fill streams, act on
commands, run actions.

## What every node gets (and can't switch off)

| | |
| --- | --- |
| Heartbeat | state, CAN state, type ID, low 16 bits of the schema's wire hash, uptime; at the `heartbeat_hz` setting |
| Node ID | asks with `id_request` (MAC + type) from 255 every 0.4–0.6 s with jitter until a host answers with `id_assign`. Or a fixed ID: `cfg.fixed_id`, or `anv_node_save_id()` (kept in storage). `cfg.save_assigned_id` keeps an assigned ID across reboots. NMT `forget_id` (while idle) asks again. |
| Outputs off by default | `outputs_off` is called at start and every time the node leaves ARMED. Outputs may be on only while `anv_node_armed()`. |
| Arm / disarm | NMT `arm` from the leader with its current term, only from IDLE |
| Command watchdog | every command with `watchdog_ms` in the schema has its own timer; while armed, a missing command disarms after `min(watchdog_ms, command_timeout_ms)`. The timer starts at ARM. |
| E-stop | an `estop` frame from anyone latches ESTOP (also without an ID); only `estop_release` from the leader with its term clears it, to IDLE. `anv_node_estop()` latches locally and tells everyone. |
| Leader tracking | follows `host_heartbeat` with role leader; highest term wins. A new term, the leader stepping down, or no heartbeat for `leader_timeout_ms` disarms. Commands, NMT, SYNC, `estop_release` and `debug_level` are taken only from the leader. |
| Fault | `anv_node_set_fault(node, true)` disarms and latches FAULT until the condition is gone **and** the leader sends NMT `fault_reset` |

States: `boot` → `unassigned` → `idle` ⇄ `armed`; `estop` and `fault` latch over everything (E-stop shows first).

## Declared by the schema, served by the runtime

- **Settings**: the protocol's common parameters (`heartbeat_hz`, `command_timeout_ms`, ...) plus the
  type's. SERVICE `get`/`set` check type, `min`/`max`, `access: ro` and `disarmed_only`; `save` writes
  them (and stream rates and the label) to storage; `restore` loads defaults (not while armed). A
  saved copy that fails its CRC is ignored; entries are matched by index, so a changed schema keeps
  what still fits.
- **Streams**: the type's `stream` messages, sent at their rate. SERVICE `get_rate`/`set_rate` (mHz)
  change it at run time within the schema's `min`/`max`; 0 = off if the schema allows it.
- **Commands**: delivered to `on_command` only while armed and only from the leader, addressed to
  this node, at least the schema's length. `apply: sync` commands are held (latest wins) and
  delivered at the leader's next SYNC.
- **Actions**: SERVICE `action` runs `on_action` (with `disarmed_only` and the argument's type checked).
- Also SERVICE `info` pages and the 16-character `label`.

## Firmware updates over CAN (`anv_node_ota.h`)

BEGIN/DATA/END/ABORT on the schema's `transfer` port (service `ota`), ACKed one message at a
time; see `anv_node_ota.h` for the exact byte layout and `host/anv_can`'s `anv-flash` for the
host side. Only accepted while the node isn't armed; BEGIN is refused if `type_id` or `chip`
doesn't match. The CRC-32 in BEGIN is checked against what was actually received before END is
allowed to apply anything, so a corrupted or interrupted transfer never touches the running
image. Node state is `updating` for the duration (E-stop and faults cancel it).

Flash access is behind `cfg.ota` (`anv_node_ota_backend_t`): `begin`/`write`/`commit`/`abort` to
move the image, `confirm`/`rollback_and_reboot` for the probation below. `NULL`: OTA always
answers `NO_BACKEND`. `anv_node_ota_esp32.h` wires it to `esp_ota_ops`; off the ESP32 it compiles
to nothing, so it can always be listed as a source file.

**Rollback** doesn't depend on ESP-IDF's own app-rollback feature: anv_node keeps its own
two-state marker in `cfg.storage`. After a committed image's first boot, the marker says so; if
it boots *again* still carrying that marker, the previous boot never confirmed itself (it
crashed, or someone power-cycled it) and `rollback_and_reboot()` runs immediately. An image
confirms itself on its own `ANV_NODE_OTA_CONFIRM_MS` after start (no application code needed) —
or sooner, if it reaches that point by surviving at all; there is nothing explicit to call.

## Diagnostics (`anv_node_diag.h`)

`anv_node_event(node, code, detail)` broadcasts the protocol's `event` message, appends it to a
small black box (the last `ANV_NODE_DIAG_LOG_CAP` events, persisted in `cfg.storage` so it
survives a reboot) and applies the `event_rate_limit` setting (same code within
`1000 / event_rate_limit` ms: dropped, bus and black box both). `anv_node_event_clear()` is the
same with the `cleared` bit set, never rate-limited. The runtime raises a few of these itself
(`params_corrupt`, `storage_write_fail`, the OTA ones above); everything else is the
application's call. A host reads the black box with the `log` transfer_service (`anv-flash log`,
or any ISO-TP client) — one request, one reply, no chunking (the box is small enough to fit one
transfer).

Debug levels are a plain setting (`debug_level`, parameter `0xF003`) plus the `debug_level`
message (leader only) to change it with an optional auto-off timer; both are handled directly by
`anv_node.c`, not this file.

## Using it

```c
#include "anv_node.h"
#include "anv_motor_controller.h"   /* generated */

static anv_node_t node;

anv_node_config_t cfg;
anv_node_config_default(&cfg);
cfg.type = &anv_motor_controller_type;
cfg.link = &can_link;                 /* started anv_link_t; the node sets its filters */
memcpy(cfg.mac, mac, 6);
cfg.storage = anv_node_nvs_ops();     /* anv_node_esp32.h; anv_node_memstore_ops() in tests */
cfg.storage_ctx = &nvs;
cfg.ota = anv_node_ota_esp32_backend(); /* anv_node_ota_esp32.h; NULL refuses OTA (NO_BACKEND) */
cfg.outputs_off = motors_off;         /* required */
cfg.on_command = on_setpoint;
cfg.fill_stream = fill_feedback;
anv_node_init(&node, &cfg);
anv_node_begin(&node, millis());

/* every loop, never blocking */
anv_node_poll(&node, millis());
```

Hooks (all optional except `outputs_off`): `setup`, `loop`, `on_command`, `fill_stream`,
`on_setting_changed`, `on_action`, `on_sync`, `on_identify`, `on_reset`, `on_state_changed`, and
`on_frame`, the raw-frame escape hatch: it gets every frame the runtime didn't handle itself (after
the safety handling has run). `anv_node_send_frame()` sends any frame with this node as the source;
`anv_node_send()` sends one of the type's own messages now.

`anv_node_esp32.h` has the ESP32 parts: NVS storage, MAC / chip / reset reason / boot count, and a
restart hook. The runtime itself has no clock, no RTOS calls and no `delay()`: the caller passes the
time to `anv_node_poll()`.

Table sizes are compile-time (`ANV_NODE_MAX_PARAMS` 64, `ANV_NODE_MAX_STREAMS` 16,
`ANV_NODE_MAX_COMMANDS` 8); `anv_node_init()` fails if a type doesn't fit. No allocation.

## Building

Same folder for all three, like `anv_link` and `anv_proto`:

- **Arduino**: pass `--library firmware/lib/anv_link --library firmware/lib/anv_proto --library
  firmware/lib/anv_node` (or put them in your libraries folder). The generated protocol code is
  compiled by the library in place; the node type's own `anv_<type>.c` is the sketch's (see
  `firmware/nodes/bench_esp32_arduino/sync_schema.sh`).
- **ESP-IDF**: add this folder to `EXTRA_COMPONENT_DIRS`.
- **PlatformIO**: `lib_deps = symlink://<path>/firmware/lib/anv_node`.

On Linux, `firmware/CMakeLists.txt` builds it and runs `firmware/lib/tests/test_node_*` on the
loopback bus: ID assignment, settings (read, write, limits, save, restore), stream rates, the
watchdog, E-stop, leader change and loss, faults and the hooks. `test_ota.c` and `test_diag.c`
cover image transfer, a corrupted image being rejected, rollback after an unconfirmed reboot,
event rate limiting, the black box surviving a reboot, and the log transfer — all against a fake
OTA backend (`node_harness.h`'s `h_ota_ops()`), since there's no real flash on Linux.
