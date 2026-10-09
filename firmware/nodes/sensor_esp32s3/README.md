# ESP32-S3 node template

An ESP-IDF project for a science/auxiliary node (node IDs 16-19, docs/architecture.md,
"Identifier scheme"): wires `acc_can_twai` (`firmware/drivers/twai`) and the shared
`anveshak_can_core` library together, sends heartbeats and an example sensor frame, and is
partitioned for OTA updates over CAN.

## Building

Requires ESP-IDF (this repo doesn't install it; CI builds this project inside the official
`espressif/idf` Docker image — see `.github/workflows/ci.yml`'s `esp32s3` job):

```sh
cd firmware/nodes/sensor_esp32s3
idf.py set-target esp32s3
idf.py build
```

## Layout

- `main/main.c` — node init, the receive/tick loop, and `send_example_sensor_frame()`.
- `main/board_config.h` — every pin this template uses (TWAI TX/RX, TCAN1044AV STB, the node
  ID switch). Edit this for a real PCB.
- `components/anveshak_can_core/` — a thin `idf_component_register()` wrapper around
  `firmware/core`'s sources; the core library itself is not copied or forked.
- `partitions.csv` — the 4 MB, two-OTA-slot layout from docs/architecture.md ("Firmware
  updates over CAN"). `sdkconfig.defaults` points the build at it and enables OTA rollback and
  flash coredumps.

## Known stubs / left for later prompts

- There is no science/auxiliary sensor CAN message in `defs/rover.dbc` yet (only drive and
  FPGA node types have one). `send_example_sensor_frame()` reuses `FPGA_SENSORS_A`'s generic
  "four u16 values" layout as a placeholder — adding a real message is a `defs/` change for a
  future prompt, owned by P0.
- `read_example_sensors()` returns a counter-derived pattern, not real sensor data. Replace it
  with actual ADC/I2C/SPI reads.
- No NVS-backed parameter storage (`anv_board_hooks_t.storage_load/storage_save` are `NULL`):
  this node type has no parameter table yet either.
- OTA, the CAN bootloader transport and debug levels are out of scope for this prompt; the
  partition table only prepares the flash layout they will use.
