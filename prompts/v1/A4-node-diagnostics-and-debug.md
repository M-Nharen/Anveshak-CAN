# A4 — Node-side error reporting and debug levels

**Who:** person A. **Needs:** A2 merged. **Folders:** `firmware/core/`, `firmware/nodes/`,
`firmware/tests/`.

## Goal

Every node reports problems as numbered events, keeps a black-box log that survives resets,
records crashes, and changes how much it sends based on the debug level.

## Read first

`docs/architecture.md` sections "Debug levels: switching debugging on and off" and "Error
reporting and diagnostics" (the node parts: NODE_EVENT, node black box, power-on self-test, crash
records, decoding CAN bus errors on STM32/ESP32).

## Build

1. **NODE_EVENT**: edge-triggered, with a "cleared" bit, rate-limited to one per code per second,
   with a sequence number. Error codes from the generated header.
2. **Black box**: a RAM ring of 64 events; errors and criticals also go to flash (STM32: reserved
   page; ESP32: NVS) keeping the last 32; boot count; SERVICE opcodes 6 LOG_READ and 7 LOG_CLEAR.
3. **Power-on self-test** with SELF_TEST_FAIL details.
4. **Crash records**: STM32 HardFault handler saving PC, LR, xPSR, CFSR, HFSR, MMFAR, BFAR to
   no-init RAM, reported after reset (HARD_FAULT event + INFO page). ESP32-S3: reset reason +
   panic PC event; core dump to its partition.
5. **CAN error-type counters** (LEC/PSR, TWAI alerts) in telemetry.
6. **Debug levels**: handle DEBUG_LEVEL (target, level, category mask, auto-off timer); report the
   level in NODE_HB bits 4–5; level 0 slows telemetry to 1 Hz; level 1 adds warnings; level 2 adds
   info events and DRIVE_DEBUG_A/B at 10 Hz; level 3 at 50 Hz; `debug_default` parameter at boot.
7. Message rates from parameters (`rate_fb_hz`, `rate_telem_hz`, `rate_hb_hz`), changeable live,
   with the safety floors (node heartbeat ≥ 5 Hz).

## Done when

- Host tests cover: rate limiting, cleared events, sequence numbers, the black box surviving a
  simulated reset, LOG_READ, debug-level behaviour per level, auto-off timer, rate changes with
  floors.
- STM32 and ESP32 builds still compile.
