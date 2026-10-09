# Gateway link wire format

Minimal change note (B4): this file documents, rather than generates, the wire format for the
UDP **gateway link** between the two hosts' gateways (docs/architecture.md, "The gateway link").
Unlike `rover.dbc`, it is not run through `defs/codegen/`: both ends are the same C++ program
(`host/gateway`), so there is no second language or process to keep in sync by hand, and the
format lives next to its one implementation in `host/gateway/include/anv_gateway/gateway_link.hpp`
and `host/gateway/src/gateway_link.cpp`. It is written down here anyway, as the shared
definitions repo asks, so a protocol change is reviewed like one.

IPv4 UDP, one port per host. All multi-byte fields little-endian.

## Header (6 bytes, every datagram)

| Byte | Field |
| --- | --- |
| 0 | Wire version (1) |
| 1 | Message type: 1 ALIVE, 2 TARGETS, 3 DEBUG |
| 2 | Sending host ID (1 Orin, 2 RPi) |
| 3 | Reserved (0) |
| 4-5 | Sequence number (u16), increments per datagram sent by that host |

## ALIVE (10 Hz; +5 bytes)

| Byte | Field |
| --- | --- |
| 0 | Role (see `anv::gw::Role`) |
| 1-2 | Term (u16) |
| 3-4 | Uptime, seconds (u16) |

## TARGETS (sent when a non-leading host has local drive targets to forward; +42 bytes: 7 per wheel × 6)

Per wheel: mode (1 byte, `anv::gw::DriveMode`), target_a (i16, ×100 like `DRIVE_CMD`), target_b
(i16, ×100), accel limit (u16, ×100).

## DEBUG (sent when a request for a debug-level change needs to reach the leader; +5 bytes)

| Byte | Field |
| --- | --- |
| 0 | Target node ID (0 = all nodes) |
| 1 | Level, 0-3 |
| 2 | Category mask |
| 3-4 | Auto-off timer, minutes (u16, 0 = until changed) |

## Liveness

Any datagram from the peer counts as a sign of life. `GATEWAY_LINK_LOST` (error code `0x8010`)
is raised after 1 s of silence, and cleared the next time a datagram arrives.
