# L1 — anv_link + anv_proto, ESP32 side

Branch: `lib/link-esp` from `next`. PR into `next`.

## Read first
- `schema/README.md` and `schema/protocol.yaml` (merged in PR #11)
- The existing bench TWAI driver and ISO-TP code in `firmware/` and `firmware/nodes/bench_esp32_arduino/`

## Build
- `anv_link` (C): a small CAN driver interface (init with bitrate, send, receive, filters,
  bus-off recovery, counters). Implementations: ESP32 TWAI, and a loopback/test driver for Linux.
  Reuse the existing TWAI driver; switch it to 29-bit extended IDs.
- `anv_proto` (C): pack/unpack the 29-bit ID fields using the widths from `protocol.yaml`
  (via the generated code, not hand-written constants), plus ISO-TP for multi-frame transfers.
  Reuse existing ISO-TP code where it fits.
- Package both as an Arduino library that also builds as an ESP-IDF component and under
  PlatformIO. No copy-and-sync scripts.

## Done when
- Unit tests run on Linux against the loopback driver: ID round-trips, ISO-TP long transfers,
  lost/out-of-order frame handling, filters.
- An example sketch builds for ESP32 in CI.
- Short README in each library folder.

## Don't
- Don't build node behaviour (heartbeat, arm, settings) — that's L3.
- Don't touch host code — that's L2.
