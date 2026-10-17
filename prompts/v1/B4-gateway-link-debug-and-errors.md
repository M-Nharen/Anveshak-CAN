# B4 — Gateway link, debug levels, event log and Errors page

**Who:** person B. **Needs:** B3 merged. **Folders:** `host/gateway/`, `host/ros2/`,
`dashboard/`.

## Goal

Let the two hosts cooperate without ROS, switch debugging on and off from the dashboard or ROS 2,
and collect every error into one event stream with a dashboard page.

## Read first

`docs/architecture.md` sections "Host software" ("The gateway link"), "Debug levels: switching
debugging on and off", and the host parts of "Error reporting and diagnostics" (gateway event log
and incident reports, the Errors page).

## Build

1. **Gateway link**: UDP between the two gateways (message definitions in `defs/`), sequence
   numbers, a 10 Hz alive message, forwarding of drive targets to the leader, debug level and
   control actions; GATEWAY_LINK_LOST after 1 s.
2. **Debug levels in the gateway**: the ROS 2 parameter `debug_level` and service
   `/can/set_debug`; the leader sends DEBUG_LEVEL; a bus-load guard that refuses changes over 50%
   (estimated from DBC rates); auto-off timers; host behaviour per level (event log file from
   level 1; raw CAN and MCAP recording from level 2).
3. **Flight recorder**: the last 60 s of raw CAN in memory, saved on a critical event or on
   request; `flight_recorder` setting, on by default.
4. **Event stream**: NODE_EVENTs plus the gateway's own codes (0x80xx), sequence-gap detection
   with automatic LOG_READ of missed events, `/can/errors` topic, JSONL log per run.
5. **Bus error decoding** from Linux error frames, with error type → likely cause text.
6. **Dashboard**: the "DEBUG ON" banner with remaining time; per-node debug selector; Errors page
   (active problems, event list with filters, node × category counts, bus errors, black boxes);
   an event timeline.

## Done when

- Tests cover: target forwarding over the link, link loss, refused debug change over 50%,
  auto-off, flight recorder saving on a critical event, gap detection triggering LOG_READ.
- ROS 2 build and tests still pass on Humble and Jazzy.
