# RPi bench: CAN gateway, node discovery and the 7" touch screen

One Raspberry Pi (Pi OS Bookworm or Ubuntu/Xubuntu desktop) with a Waveshare **2-CH CAN HAT**, a **7" 1024×600 HDMI touch screen**, and a few ESP32
boards (WROOM or S3) running `firmware/nodes/bench_esp32_arduino`. The RPi gives each board a node ID,
discovers how many are on the bus, and shows them live on the screen. It also raises an alert when a
board joins, goes missing or reboots.

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
   `bitrate 500000 sample-point 0.800` and `state ERROR-ACTIVE`.
2. Watch the bus in a terminal: `candump -td can1`. You should see `010` (the RPi's heartbeat,
   20 per second).
3. Flash **one** ESP with the bench sketch and power it on. In `candump`:
   - `5FB` = the board asking for an ID (its MAC is in the data),
   - `5F9` = the RPi answering,
   - then `680` (heartbeat of node 16, 10 per second) and `480` (its sensor frame).

   The screen shows a tile for NODE16 within a second, and the board's LED flickers green.
4. Add the other boards one by one. Each new board gets the next ID (17, 18, 19), and the same
   board always gets its own ID back, even after the RPi restarts.

## 4. Failure tests

| Do this | Expect |
| --- | --- |
| Unplug a board's CAN wires | Its tile greys out ("missing") within 300 ms; the event log and a toast say so |
| Press a board's reset button | "rebooted" alert; the tile turns yellow with a reboot count |
| Restart the gateway: `sudo systemctl restart anv-gateway` | Boards keep their IDs (table in `/var/lib/anveshak/node_ids.txt`) |
| Short CANH to CANL for a second | ESP LEDs go solid (red on the S3), then recover; the RPi's `restart-ms 100` brings `can1` back |
| Press E-STOP on the screen | `000` frame with payload `E5`; every tile shows ESTOP until released |

## Where to look when something is wrong

| Symptom | Likely cause |
| --- | --- |
| Board blinks (blue on the S3) forever, `candump` shows `5FB` but no `5F9` | Gateway not running: `journalctl -u anv-gateway` |
| `candump` shows nothing from a board | TX/RX swapped, STB not low, transceiver not powered, or wrong pins in `board_pins.h` |
| Board LED goes solid, or `ip -s link show can1` counts errors | Missing or extra termination (measure the 60 Ω), no common ground, or wrong `oscillator=` |
| `can1` missing after reboot | Overlay lines not in `config.txt`, or the HAT isn't seated |
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

The same flow runs on a virtual bus. You need `vcan` (Linux):

```sh
sudo modprobe vcan && sudo ip link add dev vcan0 type vcan && sudo ip link set vcan0 up
python host/sim/cli.py --interface socketcan --channel vcan0 --bench 4 &
anv_can_gateway --interface vcan0 --bridge-socket /tmp/gw.sock &
ANV_DASH_GATEWAY_SOCKET=/tmp/gw.sock ANV_DASH_STATIC=dashboard/frontend/dist ANV_DASH_TRUST_LOCALHOST=1 \
  python -m anv_dashboard   # from dashboard/backend; then open http://localhost:8080/?kiosk
```
