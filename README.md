# Anveshak rover CAN system

One CAN bus connects the rover's host computers (Jetson Orin, Raspberry Pi) to its nodes. **Every
node is an ESP32** (WROOM or S3). Nodes get their IDs automatically, report their health, and are
reflashed over CAN.

Spec: `docs/architecture.md`. Rules for Claude sessions: `CLAUDE.md`.

## Branches

| Branch | What |
|---|---|
| `main` | The first version (Tier 1): STM32 + ESP32 era. Kept as history. |
| `next` | **Work in progress.** The bench system (RPi + ESP32s, touch screen, flashing over CAN) with STM32 removed. The new libraries land here, one PR per `lib/*` branch, and `next` replaces `main` once they work end to end. |
| `lib/*` | One branch per library (see below). Each is a PR into `next`. |

## Where this is going: libraries first, apps on top

The code is being rebuilt as general libraries that know nothing about this particular rover (no
fixed node counts, wheel counts or node types). The rover becomes configuration plus small apps.

```
            apps (later): gateway daemon, dashboard, ROS 2 package, drive controller, ...
───────────────────────────────────────────────────────────────────────────────────
FIRMWARE (ESP32, C)                       HOST (C++ and Python)
  anv_node     node runtime, objects,       anv_host     discovery, registry, leadership,
               OTA (anv_node_ota.h) and                  parameter/command client, streams
               diagnostics (anv_node_diag.h)
  anv_proto    29-bit IDs, ISO-TP, codecs    anv_flash    flash any node, a type or a list
  anv_link     CAN driver: TWAI, test        anv_proto    same, generated from the same schema
                                             anv_link     SocketCAN, virtual, test
───────────────────────────────────────────────────────────────────────────────────
              schema/  node types in YAML → generated C, C++, Python and DBC
```

| Branch | Library |
|---|---|
| `lib/schema` | The YAML schema for node types, and the generator. **Done:** `schema/README.md` |
| `lib/link-proto` | `anv_link` + `anv_proto` (firmware and host) |
| `lib/node` | `anv_node`: node runtime and object dictionary |
| `lib/host` | `anv_host`: discovery, registry, leadership, clients |
| `lib/ota-diag` | Flashing and diagnostics on the new base |
| `lib/sim` | A simulator for any number of nodes of any type |

## Using what works today (`next`)

- **The bench** (RPi + CAN HAT + touch screen + ESP32s): `host/tools/rpi-bench/README.md`.
- **The ESP32 node sketch**: `firmware/nodes/bench_esp32_arduino/README.md`.
- **No hardware**: `host/sim` simulates nodes on a virtual bus; see "Without any hardware" in the
  bench README.
- **Flashing and diagnostics** (`anv-flash`, `anv-host`): `host/anv_can/README.md`.

## Bench demo (the v2 libraries end to end)

On the RPi bench with at least two ESP32s running the new node library (`firmware/lib`, not the
old `defs/`-based apps):

- [ ] Both nodes get IDs automatically and are listed by `anv-host list` / `anv-flash list`
- [ ] Each node reports its type and the host matches it to the schema (version check passes)
- [ ] Read and change a setting live (`anv-host get` / `set`); it survives a node reboot
- [ ] Change a stream rate live (`anv-host rate`) and see telemetry rate change
- [ ] Arm, send a command, stop sending (`anv-host arm` / `command`): node disarms within the
  watchdog timeout
- [ ] E-stop (`anv-host estop`) disarms every node immediately
- [ ] Reflash one node over CAN (`anv-flash flash`); a deliberately broken image rolls back
  on its own
- [ ] An error event raised on a node shows up on the host (`anv-host events`), and its black
  box (`anv-flash log`) still has it after a reboot

Everything above except the CAN hardware itself can be exercised on `vcan0` with `host/sim` or a
fake node, the same way the automated tests in `firmware/lib/tests` and
`host/anv_can/python/tests` do.

Earlier task prompts (the two-person P0/A/B plan) are in `prompts/v1/`.
