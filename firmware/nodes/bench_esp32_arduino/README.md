# Bench node sketch (ESP32 WROOM + ESP32-S3, Arduino)

One sketch for every ESP32 on the bench, built on the v2 libraries: `anv_link` (TWAI, 29-bit IDs),
`anv_proto` and `anv_node` (`firmware/lib`). You flash the same file everywhere: the pins come from
the library for each chip, and each board gets its node ID from the Pi's ID service or, if it has
DIP switches and is set to use them, from the switches. It is node type `esp32_bench`
(`schema/types/esp32_bench.yaml`): a `status` stream with four numbers about the board and a
`blink` action. Heartbeat, settings, stream rates, arm/disarm, E-stop and leader tracking all come
from `anv_node`.

This replaces the 11-bit sketch built on `firmware/core`: the board now speaks only the v2 protocol,
so it shows up in the v2 host tools (L4), not the old dashboard. The old sketch is on `main`.

## Flashing (Arduino IDE 2)

1. Install the **esp32 by Espressif** board package, version **3.3.0 or newer** (the `esp_twai`
   driver arrived with it).
2. Make the three libraries visible: copy or symlink `firmware/lib/anv_link`, `firmware/lib/anv_proto`
   and `firmware/lib/anv_node` into your Arduino `libraries` folder. Keep them inside the repository
   checkout (symlinks are best): they compile the generated schema code from `schema/generated/c`.
3. Open `bench_esp32_arduino.ino`, pick **ESP32 Dev Module** (WROOM) or **ESP32S3 Dev Module**, and
   upload. Open the Serial Monitor at 115200 baud.

From the Raspberry Pi (or any Linux machine set up with `host/tools/pi/setup.sh`), without the IDE:

```sh
anv-build firmware/nodes/bench_esp32_arduino --usb /dev/ttyUSB0 --chip esp32   # first time, USB
anv-build firmware/nodes/bench_esp32_arduino 17 --can can1                      # after that, CAN
```

`anv-build` fills `src/schema/` and passes the three libraries itself; see `host/tools/pi/README.md`.

## Wiring (from the library, per chip)

| Board | CAN TX | CAN RX | LED |
| --- | --- | --- | --- |
| ESP32-S3 DevKitC | GPIO 16 | GPIO 15 | RGB: the board's `RGB_BUILTIN` (GPIO 48 on most, 38 on v1.1) |
| WROOM DevKit | GPIO 4 | GPIO 5 | GPIO 2 |

Wire ESP TX to the transceiver's TXD and ESP RX to its RXD; crossing them is the classic mistake.
The TCAN's STB is tied to GND. These pins are the library's defaults for each chip
(`firmware/lib/anv_node/src/anv_board_esp32.c`). For a board wired differently, change a field of
`pins` in `setup()` before `anv_board_begin` (e.g. `pins.can_tx = 21;`, `pins.tcan_stb = 22;`).

### ESP32-S3 boards

S3s and WROOMs mix freely on the same bus. The same sketch builds for both, and the screen shows
each board's chip.

- **Board in the IDE:** **ESP32S3 Dev Module**. Leave PSRAM **Disabled** and the flash size at
  4 MB, even on 8/16 MB modules: the sketch fits, and the update slots stay the same size.
- **CAN pins:** GPIO 16 (TX) and 15 (RX). Keep clear of GPIO 19/20 (native USB), 26–32 (flash)
  and, on modules with octal PSRAM (N8R8, N16R8), 33–37. Other pins: see above.
- **Which USB port:** a DevKitC has two. Flash and watch the Serial Monitor on the one marked
  **UART** (or COM). For `Serial` on the one marked **USB**, set **Tools → USB CDC On Boot →
  Enabled**. Boards with only the native USB port need that setting. For the Pi's `.cpp` builds,
  add `ANV_DASH_FQBN_ESP32S3=esp32:esp32:esp32s3:CDCOnBoot=cdc` to `/etc/anveshak/dashboard.env`.
- **LED:** the RGB LED follows the board you pick (its `PIN_RGB_LED`, else 48). On a board whose
  LED is elsewhere, set `pins.led` in `setup()`.

## What the LED means

| WROOM (single LED) | S3 (RGB) | Meaning |
| --- | --- | --- |
| Fast blink | Red blink | Another board has the same DIP/fixed node ID: change one board's switches, then reset it |
| Slow blink | Blue blink | No node ID yet: asking the Pi's ID service (is `anv-ids@can1` running? `anv-host ids can1`) |
| Flicker | Green flicker | Running; one flash per frame the bus ACKed |
| Slow blink | White blink | Identify (NMT identify, or the `blink` action) |
| Solid on | Red | Bus-off (no ACK, short, wrong bitrate) or TWAI failed to start |

## Serial console

| Command | What it does |
| --- | --- |
| `diag` | Toggle the once-a-second status line (node state, leader, TWAI state, TEC/REC, counts) |
| `status` | Print the status line once |
| `id` | MAC, node ID and where it came from (Pi, DIP switches), type and build ID |
| `msg <text>` | Send up to 7 characters to everyone as a protocol `text` frame |
| `estop` | Send E-stop: every node latches it until the leader releases it |

Text frames from a host (`text`, port 14) addressed to this board or to everyone are printed and
echoed back to the sender with the same `seq`.

## Node IDs

Every board has an `id_source` setting:

- **`auto` (the default):** the board asks the Pi's ID service for an ID (16 and up), keeps it in
  flash and comes back with the same one after every reboot or reflash. The Pi remembers each
  board by its MAC (`anv-host ids can1` shows the table). To give a board a particular ID:
  `anv-host set-id can1 17 5` (while disarmed).
- **`dip`:** the switches are the ID, read once at boot: with four switches, IDs 1–15. Set it once
  per board: `anv-host set can1 <its ID now> id_source=dip --save`, then reset it. If the switches
  read 0, the board asks the Pi instead (and logs `dip_id_invalid`), so it always gets an ID.

The switch pins are only read on a `dip` board, so a board without switches can use those pins for
anything. Each switch goes between its pin and GND (internal pull-ups; closed = 1, bit 0 first):

| Board | DIP pins (bit 0 first) |
| --- | --- |
| ESP32-S3 | GPIO 4, 5, 6, 7 |
| WROOM | GPIO 13, 14, 25, 26 |

Hosts sit at the top, out of the way: 250 = the command-line tools, 251 = Orin, 252 = RPi,
254 = the Pi's ID service.

Two `dip` boards on the same setting: each hears the other's frames carrying its own ID, raises
`id_conflict` (visible in `anv-host events`), goes to FAULT and stops sending. The LED blinks fast
(red on the S3) and the serial console says which ID. Fix the switches and reset both.

## The `status` stream

`chip_temp` (0.01 °C), `free_heap` (KiB), `rx_frames` and `tx_frames` (low 16 bits), at 10 Hz by
default. A host can change the rate at run time (SERVICE `set_rate`, 0–50 Hz) and save it with
SAVE. Replace `hook_fill_stream()` with real sensor reads on a science board.

## `src/schema/` is a copy

The Arduino IDE only compiles files inside the sketch folder, so `src/schema/` holds a copy of this
type's generated code (`anv_esp32_bench.c/.h` and the headers it includes). Don't edit it. After
`anv-schema gen`, run `./sync_schema.sh`. CI fails if the copy is stale.

## Your own code: `user_app.cpp`

Put experiments in `user_app.cpp` (`user_setup()` once, `user_loop(now_ms)` every loop). `anv_node`
calls them from its `setup` and `loop` hooks, so the board stays a live node while they run.

The file ships with a 5 s on / 5 s off LED blink: set `USER_BLINK_DEMO` to 1 and flash. Never use
`delay()` there; use `now_ms`, because a blocked loop stops the board's CAN traffic.

## Flashing over CAN and diagnostics

Built into `anv_node` (`cfg.ota = anv_node_ota_esp32_backend();` in this sketch's `setup()`), so
nothing in `user_app.cpp` has to do anything for it. From a host: `anv-flash flash <iface> <node>
firmware.bin` (host/anv_can's README); a deliberately broken image, or one that crashes before it
confirms itself, rolls back to the previous image on its own. Error events and the black box
(`anv_node_event()`, `anv-flash log`) work the same way — see `firmware/lib/anv_node/README.md`.
USB still works exactly as before; CAN just means you don't have to unplug the board.
