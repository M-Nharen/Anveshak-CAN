# Bench node sketch (ESP32 WROOM + ESP32-S3, Arduino)

One sketch for every ESP32 on the bench. Each board gets its node ID from the RPi automatically, so
you flash the same file everywhere. Once addressed, it is a normal rover node: heartbeat at 10 Hz, a
4-value sensor frame at 10 Hz, and E-stop, NMT and leader tracking from the shared
`anveshak_can_core` (`firmware/core`).

## Flashing (Arduino IDE 2)

1. Install the **esp32 by Espressif** board package, version **3.3.0 or newer**. The sketch uses the
   `esp_twai` driver that arrived with it.
2. Open `bench_esp32_arduino.ino` and pick **ESP32 Dev Module** (WROOM) or **ESP32S3 Dev Module**.
   Keep the default partition scheme, because phase 2 (flashing over CAN) uses its two OTA slots.
3. Upload, then open the Serial Monitor at 115200 baud.

Command line: `arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/nodes/bench_esp32_arduino`.

## Wiring (`board_pins.h`)

| Board | CAN TX | CAN RX | LED |
| --- | --- | --- | --- |
| ESP32-S3 DevKitC | GPIO 16 | GPIO 15 | RGB on GPIO 48 (38 on some revisions) |
| WROOM DevKit | GPIO 4 | GPIO 5 | GPIO 2 |

Wire ESP TX to the transceiver's TXD and ESP RX to its RXD; crossing them is the classic mistake. The TCAN's STB is tied to GND. If a board
drives it from a GPIO instead, set `BENCH_TCAN_STB_GPIO`.

## What the LED means

| WROOM (single LED) | S3 (RGB) | Meaning |
| --- | --- | --- |
| Fast blink | Blue blink | No node ID yet; asking the RPi |
| Flicker | Green flicker | Running; one flash per frame the bus ACKed |
| Solid on | Red | Bus-off (no ACK, short, wrong bitrate); recovering |

If a board blinks blue forever, it isn't hearing an answer. Check that the gateway is running on
the RPi (`candump can0` should show `5FB` frames from the board) and check the wiring and termination.

## Serial console

| Command | What it does |
| --- | --- |
| `diag` | Toggle the once-a-second status line (TWAI state, TEC/REC, bus errors, counts) |
| `status` | Print the status line once |
| `id` | MAC, node ID and build ID |
| `forget` | Erase the saved node ID and restart, so the board asks for a new one |
| `msg <text>` | Send up to 7 characters to the RPi; they show up in the touch screen's Messages view |
| `raw <text>` | Send up to 8 characters on ID 0x7FF. Other bench boards print it, as the old test sketch did |

## Messages from the screen

The touch screen's **Messages** view sends up to 7 characters to one board or to all of them. The board
prints `Message from RPi: <text>` here and echoes the text back, and the screen ticks the message as
received by that node. Text from `msg <text>` goes the other way.

## How IDs are assigned

The board sends `NODE_ID_REQUEST` (0x5FB) with its MAC every ~0.5 s, with random jitter. The
gateway answers with an ID from 16–19, then 22–30, and remembers the MAC in its node ID table, so the
board always gets the same ID back. The board saves the ID in flash (NVS) and restarts as that node.
Details: `docs/architecture.md`, "Automatic node IDs".

To give a board a new ID: run `forget` on the board, and free the old ID on the RPi by long-pressing
its tile on the screen, or with the bridge command `forget_node`.

## The four sensor values

1. Chip temperature in 0.01 °C
2. Free heap in KiB
3. Frames received (low 16 bits)
4. Frames sent and ACKed (low 16 bits)

Replace `send_sensor_frame()` with real sensor reads on a science board.

## `src/anv/` is a copy

The Arduino IDE only compiles files inside the sketch folder, so `src/anv/` holds a copy of
`firmware/core` and the generated CAN code. Don't edit it. After changing `firmware/core` or
regenerating `defs/`, run `./sync_core.sh`. CI fails if the copy is stale.

## Not done yet

- No hardware acceptance filters; software filtering is enough at bench traffic levels. See the
  note in `twai_arduino_driver.c` before using these boards on the full rover.
- Flashing over CAN comes in phase 2.
