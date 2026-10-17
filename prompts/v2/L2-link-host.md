# L2 — anv_link + anv_proto, host side (C++ with Python bindings)

Branch: `lib/link-host` from `next`. PR into `next`.

## Read first
- `schema/README.md`, `schema/protocol.yaml`, and the generated JSON schema output
- The existing gateway CAN code in `host/gateway/`

## Build
- `anv_link` (C++): same interface idea as the firmware side. Implementations: SocketCAN
  (real and vcan), and an in-process test bus.
- `anv_proto` (C++): 29-bit ID packing driven by the generated schema JSON, message
  encode/decode by name, and ISO-TP. Must produce byte-identical frames to the C firmware side
  (reuse the schema's cross-check test vectors).
- Thin Python bindings (pybind11 or similar) so tools and the dashboard can use the same code.

## Done when
- C++ tests pass against the test bus; Python tests pass using the bindings.
- A small CLI (`anv-can dump`, `anv-can send`) decodes live traffic on vcan0 using the schema.
- Works on Raspberry Pi OS and on Ubuntu (Jetson). CI builds it.

## Don't
- Don't build discovery, leader election or parameter clients — that's L4.
