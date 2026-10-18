# anv_link

A small CAN driver interface for the v2 libraries: classic CAN with **29-bit extended IDs only**.
Every backend sits under one front end (`anv_link_t`), which keeps what every backend would otherwise
repeat:

- **start** with a bitrate (a setting, default 500 kbit/s) and up to 4 acceptance filters
- **send / receive**, never blocking; receive returns only frames that pass the filters
- **filters** (`id`/`mask` pairs), changeable at run time, e.g. once the node has an ID. Backends
  may filter in hardware; the front end applies the exact filters again in software
- **counters**: frames sent, refused, failed, received, filtered, lost to overflow, bus-offs, recoveries
- **bus-off recovery**: `anv_link_poll()` notices bus-off and starts recovery by itself (retrying every
  `recover_retry_ms`), and returns `ANV_LINK_EV_*` flags so the node can raise the protocol's
  `can_*` events. On the ESP32 the TWAI backend recovers by rebuilding the controller (after
  `ANV_TWAI_RESTART_HOLDOFF_MS` of bus-off), not with ESP-IDF's in-place recovery, which on the
  classic ESP32 could leave a node that reports error-active but neither sends nor ACKs

Backends:

| File | What |
| --- | --- |
| `anv_link_twai.c` | ESP32 / ESP32-S3 TWAI over ESP-IDF 5.5's `esp_twai` (Arduino-ESP32 3.3+). Compiles to nothing elsewhere. |
| `anv_link_loopback.c` | In-process bus for Linux tests. A tap sees every frame and can drop, change or reorder it; error states and receive overflow can be forced. |
| `anv_link_twai_filter.c` | How filters map onto the single TWAI acceptance filter (two filters use dual mode, which compares ID bits 28..13, where `dst` lives). Plain C, tested on Linux. |

```c
static anv_twai_t twai;
static anv_link_t can_link;

anv_twai_setup(&twai, TX_GPIO, RX_GPIO);
anv_link_init(&can_link, anv_twai_ops(), &twai);
anv_link_config_t cfg;
anv_link_config_default(&cfg);
anv_link_start(&can_link, &cfg);

// every loop
anv_link_frame_t f;
while (anv_link_receive(&can_link, &f)) { /* ... */ }
uint32_t ev = anv_link_poll(&can_link, millis());
```

## Building

The same folder is all three:

- **Arduino**: a library (`library.properties`, sources in `src/`). Put it in your libraries folder
  or pass `--library firmware/lib/anv_link` to `arduino-cli`.
- **ESP-IDF**: a component (`CMakeLists.txt`); add this folder to `EXTRA_COMPONENT_DIRS`.
- **PlatformIO**: a library (`library.json`); `lib_deps = symlink://<path>/firmware/lib/anv_link`.

On Linux, `firmware/CMakeLists.txt` builds it with the loopback backend and runs the tests in
`firmware/lib/tests`. The example sketch is in `anv_proto/examples/isotp_echo`.

Not here (L3): node behaviour such as heartbeats, arming and settings.
