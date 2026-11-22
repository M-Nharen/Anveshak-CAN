# Build and flash ESP32 nodes from the Raspberry Pi

Everything happens on the Pi: edit a sketch, build it, flash it. The first flash of a new board goes
over USB, and every flash after that goes over CAN. You don't need a laptop, the Arduino IDE, a
`libraries` folder or hand-copied schema files.

| Step | Command | How long |
| --- | --- | --- |
| Once per Pi | `host/tools/pi/setup.sh` | 20–40 min (downloads ~4 GB, builds the host library) |
| Once per new board (USB cable to the Pi) | `anv-build <sketch> --usb /dev/ttyUSB0 --chip esp32` | 1–3 min |
| Every change after that | `anv-build <sketch> <node> --can can1` | 30 s–2 min |

"OTA" and "flashing over CAN" are the same thing here: the image travels over the CAN bus
(`anv_node`'s OTA service) into the node's spare app slot. The node checks the CRC, reboots into it,
and rolls back by itself if the new image crashes within 5 s.

## 1. Set up the Pi (once)

You need **64-bit Raspberry Pi OS** (Raspberry Pi Imager > "Raspberry Pi OS (64-bit)"), a Pi 4 or 5
with 2 GB+ RAM, and 6 GB+ free space. Then:

```sh
git clone https://github.com/M-Nharen/Anveshak-CAN.git ~/Anveshak-CAN
cd ~/Anveshak-CAN && git checkout esp-dip-ids
host/tools/pi/setup.sh
```

The script installs and sets up:

- `arduino-cli` 1.3.1 in `~/.local/bin`, with the ESP32 core **3.3.12** (the version CI builds with).
- The host library in `~/anv-venv`, with `anv-build`, `anv-flash`, `anv-host`, `anv-can` and
  `anv-schema` linked into `~/.local/bin`.
- `ANV_REPO` pointing at your checkout, so `anv-build` finds the libraries from any folder, and
  `ANV_SCHEMA_JSON` pointing at the repo's `schema/generated/schema.json`, so `anv-host`, `anv-can`
  and your Python scripts know a new node type after a `git pull`. You don't need to reinstall.
- Your user in the `dialout` group, for USB flashing (log out and in once).
- `can0` and `can1` brought up at boot at 500 kbit/s, restarting by themselves after a bus-off.
  If they don't exist yet, it prints the `config.txt` lines for the CAN HAT.
- One test build of the bench sketch for both chips. This takes the longest, but later builds
  reuse its compiled core.

Run it again after a `git pull` that changes `host/`. It only redoes what's needed.

**On 32-bit Raspberry Pi OS** the script stops. That OS usually runs a 64-bit kernel, so
`arduino-cli` downloads the 64-bit ESP32 compiler, which can't start on the 32-bit system. The error
names `ld-linux-aarch64.so.1`. The fix is the 64-bit OS.

## 2. Build and flash

```sh
anv-build --check                                                  # arduino-cli + esp32 core ready?

# just build (the .bin goes in ~/.cache/anveshak/build/<sketch>-<chip>/build/)
anv-build firmware/nodes/bench_esp32_arduino --chip esp32s3 -o ~/

# a new board on USB: build and upload (ESP32 DevKit: /dev/ttyUSB0; S3 native USB: /dev/ttyACM0)
anv-build ~/my_node --usb /dev/ttyUSB0 --chip esp32

# over CAN: asks each node its chip, builds once per chip, flashes one by one
anv-build ~/my_node 17 --can can1
anv-build ~/my_node 17,18,19 --can can1
anv-build ~/my_node type:wheel --can can1
anv-build ~/my_node all --can can1
```

What `anv-build` does, so you don't have to:

1. **Regenerates the schema** (`anv-schema gen`) if you changed a YAML in `schema/types/`.
2. **Fills `src/schema/`.** It looks for `#include "src/schema/anv_<type>.h"` in your sketch and copies
   that type's generated files, plus `anv_schema.h` and `anv_protocol.h`, into a work copy. Your
   sketch folder isn't touched, and a stale copy can't happen.
3. **Passes the libraries with `--library`** from where they are in the repo. They must stay there,
   because they compile the generated protocol code in place. This is why copying them into an
   Arduino `libraries` folder was fiddly.
4. **Keeps a build folder per sketch and chip**, so the second build only compiles what changed.
5. Over CAN, it never takes the lead or arms anything, and it refuses to flash an armed node.

A sketch can live anywhere, for example `~/my_node/my_node.ino` (the folder and the `.ino` must have
the same name). Start from `firmware/nodes/bench_esp32_arduino` or the guide's examples.

**Board options**, for example `Serial` on the S3's native USB port: pass
`--fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc`, or set it once with
`export ANV_FQBN_ESP32S3=esp32:esp32:esp32s3:CDCOnBoot=cdc` in `~/.profile`.

## 3. When something goes wrong

| Message | Fix |
| --- | --- |
| `arduino-cli not found` | Run `setup.sh`, or open a new terminal so `~/.local/bin` is on `PATH` |
| `esp32 core isn't installed` | Run `setup.sh`, or `arduino-cli core install esp32:esp32@3.3.12` |
| `64-bit compiler can't run on this 32-bit OS` | Install 64-bit Raspberry Pi OS (see section 1) |
| `includes no node type` | Add `#include "src/schema/anv_<type>.h"` to the sketch |
| `no generated code for type(s) ...` | The YAML isn't in `schema/types/`, or `anv-schema check` shows an error in it |
| `no access to the USB port` | `sudo usermod -aG dialout $USER`, then log out and in |
| USB upload hangs at `Connecting...` | Hold BOOT on the board while it connects; try the other USB port on an S3 |
| `node N: not heard on can1` | Wrong ID (check the DIP switches), board not on the bus, or `can1` down: `ip -details link show can1` |
| `node N: armed, skipping` | Disarm it first: `anv-host disarm can1 N --take-lead` |
| `wrong type or chip` | The sketch is for another node type than the board runs. To change a board's type, flash it over USB once |
| Out of memory while compiling (Pi with 1 GB) | Add swap: `sudo dphys-swapfile swapoff; sudo sed -i 's/^CONF_SWAPSIZE=.*/CONF_SWAPSIZE=2048/' /etc/dphys-swapfile; sudo dphys-swapfile setup; sudo dphys-swapfile swapon` |

`anv-build -v` shows the full compiler output.
