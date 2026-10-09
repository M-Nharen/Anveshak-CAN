# A1 — Firmware core library (`anveshak_can_core`)

**Who:** person A. **Needs:** P0 merged. **Folders:** `firmware/core/`,
`firmware/drivers/loopback/`, `firmware/tests/`, the firmware job in `.github/workflows/ci.yml`.

## Goal

The shared C library every node runs: CAN abstraction, node state machine, watchdogs,
heartbeat, master tracking, command handling, parameters and NMT. Fully tested on Linux, with
no hardware.

## Read first

`docs/architecture.md` sections: "Node firmware", "Hosts: choosing which one leads" (only "What
nodes do" and "Host heartbeat and the claim rules"), "Safety and fault handling" ("Checks every
node makes on every command"), and the NMT, HOST_HB, DRIVE_CMD, NODE_HB and SERVICE layouts in
"Message catalog".

## Build

1. **CAN driver interface** (`firmware/core/include/acc_can.h`): init (bitrate, two mask filters),
   send (with priority), receive (from an ISR-fed ring buffer), error state (TEC, REC, bus-off),
   start bus-off recovery. A **loopback** driver in `firmware/drivers/loopback/` simulates a bus
   with several nodes in one process, for tests.
2. **Software transmit queue** with priority (E-stop > feedback > telemetry > service), and a
   receive ring buffer.
3. **Node state machine**: BOOT, DISARMED, ARMED, FAULT, ESTOP, BOOTLOADER, with exactly the
   transitions in the "Node states" table.
4. **Master tracking** from HOST_HB (role, term). Disarm on term change. Ignore commands whose
   sender or term doesn't match the current master. Treat a frozen sequence counter as a lost
   master.
5. **Watchdogs**: 100 ms command watchdog; a hook for the hardware watchdog (the board feeds
   it); 300 ms "no master" flag.
6. **SYNC handling**: store the latest command; apply it at the next SYNC.
7. **Heartbeat** (NODE_HB at the rate parameter) and **telemetry scheduling** from parameter
   rates.
8. **Parameters**: table from `defs/params` (generated), GET / SET / SAVE / RESTORE_DEFAULTS /
   INFO over SERVICE, with per-requester sub-types. The "disarmed only" rule enforced. Storage
   behind an interface (flash on boards, a file or RAM in tests).
9. **NMT**: ARM, DISARM, RESET, ENTER_BOOTLOADER (sets a flag through a board hook), FAULT_RESET.
10. **ESTOP**: latch on 0x000; release only on ESTOP_RELEASE from the master, and the node then
    stays DISARMED until ARM.
11. **Board hooks** the application implements: read node ID switch, set outputs off, feed
    watchdog, now-in-ms, persistent storage.

## Tests (`firmware/tests/`, CMake + a small C test framework like Unity or a minimal one)

- Arms only from the current master; ignores other hosts' commands.
- Disarms within 100 ms of the last command; disarms on term change.
- Applies commands only at SYNC.
- ESTOP latches; release + ARM needed to move.
- Parameter SET refused while armed for protected parameters.
- Bus-off: the node keeps outputs off and recovers through the driver hook.

## Done when

- `cmake -B build firmware && cmake --build build && ctest --test-dir build` passes on Linux.
- The core also compiles with `arm-none-eabi-gcc` for a Cortex-M4 target, with no warnings at
  `-Wall -Wextra`.
- The CI firmware job runs both.

## Out of scope

Real STM32 or ESP32 drivers (A2), bootloader (A3), events and debug levels (A4).
