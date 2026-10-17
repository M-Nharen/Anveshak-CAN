# B5 — Control panel: rates, parameters, firmware and tuning pages

**Who:** person B. **Needs:** B3 merged. **Folders:** `dashboard/`, `host/gateway/` (only what the
pages need).

## Goal

Change how the rover behaves from the website: message rates, parameters, firmware versions and
live PID tuning, with the bus-load guard and audit log.

## Read first

`docs/architecture.md` section "Master dashboard", parts 3 (Rates page), 5 (Live tuning) and
7 (Audit and undo) of "Control panel", plus the Parameters, Firmware and Test bench rows of the
"Pages" table.

## Build

1. **Rates page**: every periodic message with current rate, allowed range and a selector; a
   projected bus-load bar before applying; safety floors (host heartbeat ≥ 20 Hz, node heartbeat
   ≥ 5 Hz, control cycle 25–50 Hz); "Save as default". Node rates go through SERVICE parameter
   writes; host rates are gateway settings.
2. **Parameters page**: per-node table from `defs/params`, get / set / save / restore defaults,
   "disarmed only" respected.
3. **Firmware page**: build ID for every node, mismatches flagged; flash button calling
   `anv-flash` (from A3, if merged; otherwise leave the button disabled with a note).
4. **Test bench**: single-motor jog (hold-to-run) and step-response plot with PID gain sliders.
5. **Audit and undo**: every change logged as CONTROL_ACTION with old and new value;
   CHANGE_REFUSED when the guard says no; "Undo last change".
6. ROS 2 services `/can/set_rate`, `/can/take_control`.

## Done when

- Tests cover: the load guard refusing an over-50% change, floors enforced, undo restoring the
  previous value, and parameter writes refused while armed for protected parameters.
- Frontend builds in CI.
