# B6 — Frame lab, profiles and incident reports

**Who:** person B. **Needs:** B4 and B5 merged. **Folders:** `dashboard/`, `host/gateway/`,
`defs/profiles/`.

## Goal

The advanced debugging tools: compose, send and override frames from the website, apply named
profiles in one click, and produce incident-report bundles.

## Read first

`docs/architecture.md`: "Control panel" parts 4 (Frame lab) and 6 (Profiles), and "Gateway event
log and incident reports" in "Error reporting and diagnostics".

## Build

1. **Frame lab** (debug level ≥ 2 only): a DBC-driven form for any message (signals by name,
   unit, range) or raw ID + hex; send once or repeat at a rate for a time. Overrides on signals in
   the gateway's own frames, highlighted on every page, with expiry. Refuse IDs owned by nodes;
   refuse classes 0, 1 and 7 while armed (except ESTOP). Everything goes out through the
   leader's gateway. ROS 2 services `/can/send_frame` and `/can/set_override`.
2. **Profiles**: YAML files in `defs/profiles/` (Competition, Testing, PID tuning, Bench)
   setting rates, debug levels, speed caps and limits; apply from the dashboard header or
   `/can/apply_profile`, through the load guard.
3. **Incident reports**: a zip with the raw CAN window (30 s before to 10 s after), gateway
   events, node snapshots, black boxes (via LOG_READ), parameters, crash records decoded to
   source lines (addr2line against the CI-built ELF for that build ID, if available), and an
   auto-written summary page. Created on critical events (when the debug level or flight
   recorder allows) and on demand.

## Done when

- Tests cover: refused node-owned IDs, refused command frames while armed, override expiry,
  profile application through the guard, and a generated incident bundle containing every part.
