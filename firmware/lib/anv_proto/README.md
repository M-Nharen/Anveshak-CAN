# anv_proto

The v2 protocol's identifier and multi-frame transfers, on top of [`anv_link`](../anv_link).

## 29-bit identifier (`anv_proto.h`)

`priority | dst | port | src`. The widths and positions come from the generated
`schema/generated/c/anv_protocol.h` (from `id_layout` in `schema/protocol.yaml`). Nothing here
hard-codes them, and a compile-time check fails if the layout stops adding up to 29 bits.

```c
anv_proto_addr_t a = {ANV_SERVICE_PRIORITY, dst, ANV_SERVICE_PORT, self};
anv_link_frame_t f;
anv_proto_frame(&f, &a, payload, 8);          // false if a field doesn't fit its width
anv_proto_addr_t back = anv_proto_addr(f.id);  // and back

anv_link_filter_t flt[2];
anv_link_set_filters(&can_link, flt, anv_proto_node_filters(self, flt));  // "to me" + broadcast
```

## ISO-TP (`anv_isotp.h`)

ISO 15765-2 on classic CAN: single frames up to 7 bytes, or first frame plus consecutive frames up to
4095 bytes. Flow control has block size and STmin (settings), WAIT frames, and overflow. One
`anv_isotp_t` is one channel to one peer on one port (default: protocol.yaml's `transfer` port and
priority). It can send and receive at the same time. There's no allocation and no clock: you pass
the time in µs, feed it received frames, and call `anv_isotp_poll()` every loop.

Lost, repeated or reordered consecutive frames abort the receive with `ANV_ISOTP_ERR_WRONG_SN`.
Silence aborts with timeouts (`TIMEOUT_CF` on the receiver, `TIMEOUT_FC` on the sender). The channel
is ready for the next transfer straight away. ISO-TP has no end-to-end acknowledgement, so the
protocols on top (OTA, logs, L5) confirm with their own replies.

Built from the receive side of the bench sketch's `ota_can.cpp`, made symmetric and independent of
OTA.

## Building

Like `anv_link`, this folder is an Arduino library, an ESP-IDF component and a PlatformIO library.
It needs `anv_link`. The generated header is used where it is in this repository: CMake and ESP-IDF
add `schema/generated/c` to the include path. Arduino can't do that for a library, so
`anv_proto_schema.h` falls back to a path relative to itself. That means the library must stay
inside this repository, not be copied into an Arduino libraries folder. Point `arduino-cli` at it
instead:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32s3 \
  --library firmware/lib/anv_link --library firmware/lib/anv_proto \
  firmware/lib/anv_proto/examples/isotp_echo
```

`firmware/lib/build_checks/` holds the ESP-IDF project and PlatformIO project CI uses to build the
same code those two ways.

## Example

`examples/isotp_echo`: flash it on two boards, then type `send <id> 4000` in the Serial Monitor.
The other board echoes the transfer back and the first board checks it.

## Tests (Linux)

```sh
cmake -S firmware -B build && cmake --build build && ctest --test-dir build -R lib_
```

`test_ids` (round trips, field isolation, filters, TWAI hardware filter mapping), `test_link`
(delivery, filters, counters, overflow, bus-off recovery), `test_isotp` (long transfers, block size,
STmin, lost / duplicated / reordered frames, timeouts, overflow, WAIT, link refusing frames).
