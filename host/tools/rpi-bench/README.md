# RPi bench: CAN gateway, node discovery and the 7" touch screen

One Raspberry Pi (Pi OS Bookworm or Ubuntu/Xubuntu desktop) with a Waveshare **2-CH CAN HAT**, a **7" 1024×600 HDMI touch screen**, and a few ESP32
boards (WROOM or S3) running `firmware/nodes/bench_esp32_arduino`. The RPi gives each board a node ID,
discovers how many are on the bus, and shows them live on the screen. It also raises an alert when a
board joins, goes missing or reboots.

**This branch runs the v2 libraries** (29-bit IDs, `schema/`): the always-running gateway is
`anv-bench-gateway` (`host/apps/bench_gateway`), built on `anv_can`, and the boards run the v2 bench
sketch. The screen looks and works as before. `install.sh --v1` puts the old 11-bit gateway back.

```
RPi + CAN HAT can1 (120 Ω on) ──── ESP32 #1 ──── ESP32 #2 ──── … ──── ESP32 #n (120 Ω on)
        CANH / CANL / GND, one daisy-chained twisted pair
```

## 1. Wire the bus

1. **Termination: exactly two 120 Ω resistors, one at each end.** On the bench the RPi is one end,
   so leave the 120 Ω jumper **on** for the HAT channel the bus is wired to (the one Linux calls
   `can1`, see below). Keep the 120 Ω on the last ESP's transceiver
   board and **remove it from every board in between**. Many transceiver breakouts have one
   soldered on (often marked R2 or 121).
2. **Check with a multimeter**, everything powered off, across CANH and CANL anywhere on the bus:
   ~60 Ω is right. ~120 Ω means one terminator is missing; ~40 Ω means one too many.
3. **Ground.** The 2-CH CAN HAT is isolated, so its CAN side doesn't share the RPi's ground. Wire
   the HAT's **GND terminal** to the bus ground wire, and run that ground to every ESP board.
4. **TCAN transceivers.** STB is tied to GND (normal mode). If yours are TCAN1044-type parts, VCC
   needs **5 V** and VIO 3.3 V. 3.3 V-only TCAN parts just need 3.3 V.
5. ESP TX → transceiver TXD, ESP RX → RXD. Pins are in the sketch's `board_pins.h`:
   S3 = TX 16 / RX 15, WROOM = TX 4 / RX 5.

## 2. Set up the RPi (once)

```sh
git clone <this repo> ~/Anveshak-CAN && cd ~/Anveshak-CAN
sudo host/tools/rpi-bench/install.sh
sudo reboot
```

`install.sh` does the following:

- Installs the build tools and `can-utils`.
- Adds the CAN HAT overlays to `/boot/firmware/config.txt`, keeping a backup in `config.txt.bak-anveshak`. Pass
  `--no-overlays` to skip this step and add the lines yourself:
  ```
  dtparam=spi=on
  dtoverlay=mcp2515-can0,oscillator=16000000,interrupt=23
  dtoverlay=mcp2515-can1,oscillator=16000000,interrupt=25
  ```
  Look at the crystal on the HAT. If it isn't marked 16.000 MHz, change `oscillator=` to match, or
  nothing will talk at 500 kbps.
- Builds the gateway into `/opt/anveshak/bin` with the config `/etc/anveshak/gateway.yaml` (a copy of
  `host/gateway/config/rpi-bench.yaml`).
- Installs the dashboard (Python venv in `/opt/anveshak`, frontend built with npm). It creates a
  password in `/etc/anveshak/dashboard.env`, which other machines need; the touch screen doesn't.
- Enables three services: `anv-can@can1` (500 kbps, 80 % sample point, auto-restart after bus-off),
  `anv-gateway` and `anv-dashboard`.

**Which interface:** the bench bus is on the interface Linux names **`can1`**. The overlay order means
Linux's names don't follow the labels printed on the HAT. That's the default. If yours turns out to
be `can0`, run `sudo host/tools/rpi-bench/install.sh --iface can0`. To find out, bring both up and
see which one shows the ESPs' frames in `candump`.
- Adds a desktop autostart entry, so the Bench page opens full-screen when you log in.

Run it again after a `git pull` to rebuild and restart.

## 3. Bring it up, one board at a time

1. After the reboot, check the interface: `ip -details link show can1` should show
   `bitrate 500000` and `state ERROR-ACTIVE`. Check the gateway with `systemctl status anv-gateway`.
2. Watch the bus decoded: `anv-can dump can1`. You should see `host_heartbeat` from node 2 (the RPi,
   20 per second). Plain `candump -td can1` works too, but the 29-bit IDs are harder to read.
3. Flash **one** ESP over USB with the v2 bench sketch. In the Arduino IDE, link the libraries first,
   as `firmware/nodes/bench_esp32_arduino/README.md` describes. Power it on. In `anv-can dump`:
   - `id_request` from 255 = the board asking for an ID (its MAC is in the data),
   - `id_assign` from 2 = the RPi answering,
   - then `heartbeat` and `status` from node 16, 10 per second each.

   The screen shows a tile for NODE16 within a second, with its type, chip and live values.
4. Add the other boards one by one. Each new board gets the next ID (17, 18, ...), and the same
   board always gets its own ID back, even after the RPi restarts (`/var/lib/anveshak/node_ids_v2.txt`).

## 4. Failure tests

| Do this | Expect |
| --- | --- |
| Unplug a board's CAN wires | Its tile greys out ("missing") within 300 ms; the event log and a toast say so |
| Press a board's reset button | "rebooted" alert; the tile turns yellow with a reboot count |
| Restart the gateway: `sudo systemctl restart anv-gateway` | Boards keep their IDs (table in `/var/lib/anveshak/node_ids_v2.txt`) |
| Short CANH to CANL for a second | ESP LEDs go solid (red on the S3), then recover; the RPi's `restart-ms 100` brings `can1` back |
| Press E-STOP on the screen | An `estop` frame; every tile shows ESTOP until the leader releases it |

## 5. Flash a node over CAN

On the screen (or at `http://<rpi>:8080/#bench` from a laptop), tap **Flash…** on the node's tile and
pick either of these:

- **A `user_app.cpp`**: your code for the board (`firmware/nodes/bench_esp32_arduino/user_app.cpp` is
  the template). The Pi builds the bench sketch with it for that node's chip (WROOM or S3), then
  flashes it. The first build takes a few minutes. Uploading **the same file** again reuses that build
  and goes straight to flashing; builds are kept in `~/.cache/anveshak/builds`. Compiler errors show
  in the dialog.
- **A ready `.ino.bin`** (Arduino IDE: **Sketch → Export Compiled Binary**; not the `.merged.bin`).

The RPi refuses an image for the other chip, and never flashes an armed node. A banner and the tile
show the progress. When the dialog says the node restarted, it's running the new image. If the new
image crashes, the board rolls back to the old one by itself.

**One-time setup for `.cpp` uploads**, as the desktop user (not root):

```sh
mkdir -p ~/.local/bin
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=~/.local/bin sh
~/.local/bin/arduino-cli config init
~/.local/bin/arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
~/.local/bin/arduino-cli core update-index
~/.local/bin/arduino-cli core install esp32:esp32@3.3.12
```

Then re-run `sudo host/tools/rpi-bench/install.sh` so the dashboard knows where `arduino-cli` is.

**From a terminal on the Pi** (the v2 tools; the interface comes first):

```sh
anv-flash list can1                                    # nodes: type, state, build, chip
anv-flash flash can1 16 bench_esp32_arduino.ino.bin    # one node
anv-host list can1                                     # nodes and hosts
anv-host events can1                                   # error events from every node
```

## Where to look when something is wrong

| Symptom | Likely cause |
| --- | --- |
| Board blinks (blue on the S3) forever; `anv-can dump can1` shows `id_request` but no `id_assign` | Gateway not running: `journalctl -u anv-gateway` |
| `candump` shows nothing from a board | TX/RX swapped, STB not low, transceiver not powered, or wrong pins in `board_pins.h` |
| Board LED goes solid, or `ip -s link show can1` counts errors | Missing or extra termination (measure the 60 Ω), no common ground, or wrong `oscillator=` |
| `can1` missing after reboot | Overlay lines not in `config.txt`, or the HAT isn't seated |
| Board never shows up, but `anv-can dump` shows 11-bit frames from it | It still runs the old (11-bit) sketch: flash the v2 sketch once over USB |
| Flash fails with "arduino-cli not found" or an esp32 error | Do the one-time `.cpp` setup in section 5, then re-run `install.sh` |
| Screen stays white / "backend offline" | `systemctl status anv-dashboard`; the kiosk waits for `http://localhost:8080/api/health` |

**To give a board a new ID:** type `forget` in its serial console, then long-press its old tile and
choose **Forget ID** to free the old number.

**Opening the screen by hand:** `pkill -f anv-kiosk; /opt/anveshak/bin/anv-kiosk.sh &`. This works from
a terminal on the Pi's desktop, or over SSH while someone is logged in to the desktop; it then uses
display `:0`. To quit kiosk mode run `pkill -f anv-kiosk; pkill chromium`, because the script
restarts Chromium if it closes.

**Without the touch screen:** open `http://<rpi-address>:8080/#bench` from a laptop on the same
network. Use the normal tabs, or add `?kiosk` for the full-screen layout.

## Without any hardware

The gateway's tests run it against fake v2 nodes on an in-process bus
(`pytest host/apps/bench_gateway/tests`, after `pip install ./host/anv_can ./host/apps/bench_gateway`).
On a Linux machine with `vcan` you can run the real thing:

```sh
sudo modprobe vcan && sudo ip link add dev vcan0 type vcan && sudo ip link set vcan0 up
anv-bench-gateway --interface vcan0 --socket /tmp/gw.sock &
ANV_DASH_GATEWAY_SOCKET=/tmp/gw.sock ANV_DASH_STATIC=dashboard/frontend/dist ANV_DASH_TRUST_LOCALHOST=1 \
  python -m anv_dashboard   # from dashboard/backend; then open http://localhost:8080/?kiosk
```

Nodes then come from real boards on a USB-CAN adapter, or a v2 simulator once `lib/sim` lands.
