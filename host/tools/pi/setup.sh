#!/usr/bin/env bash
# One-time setup of a Raspberry Pi (or any Debian/Ubuntu machine, e.g. the Jetson) as the place
# where ESP32 firmware is built and flashed: arduino-cli + the ESP32 core, the host library and
# its tools (anv-build, anv-flash, anv-host, anv-can, anv-schema), USB serial access, and the CAN
# interfaces brought up at boot. Safe to run again (after a `git pull`, it updates the tools).
#
#   host/tools/pi/setup.sh                    # everything, CAN interfaces can0 and can1
#   host/tools/pi/setup.sh --can "can1"       # only bring up can1 at boot
#   host/tools/pi/setup.sh --no-warm          # skip the first (slow) test build
#
# Run it as your normal user, from anywhere inside the repository; it asks for sudo when needed.
set -euo pipefail

ARDUINO_CLI_VERSION=1.3.1
ESP32_CORE=esp32:esp32@3.3.12   # keep in step with .github/workflows/ci.yml
ESP32_INDEX=https://espressif.github.io/arduino-esp32/package_esp32_index.json
BITRATE=500000
VENV="$HOME/anv-venv"
BIN="$HOME/.local/bin"

can_ifaces="can0 can1"
warm=1
while [[ $# -gt 0 ]]; do
  case "$1" in
    --can) can_ifaces="$2"; shift 2 ;;
    --no-warm) warm=0; shift ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

step() { printf '\n== %s\n' "$*"; }
die() { printf '\nsetup: %s\n' "$*" >&2; exit 1; }

[[ $EUID -ne 0 ]] || die "run this as your normal user, not with sudo (it asks for sudo itself)"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
[[ -d $repo/firmware/lib/anv_node ]] || die "can't find the repository from $0"

step "Checking the machine"
machine="$(uname -m)"
userland="$(dpkg --print-architecture 2>/dev/null || echo unknown)"
echo "kernel: $machine, userland: $userland"
case "$machine/$userland" in
  aarch64/arm64) cli_arch=ARM64 ;;
  x86_64/amd64) cli_arch=64bit ;;
  aarch64/armhf)
    # The classic trap: 32-bit Raspberry Pi OS on a 64-bit kernel. arduino-cli sees aarch64 and
    # downloads the 64-bit ESP32 compiler, which can't start here (ld-linux-aarch64.so.1 missing).
    die "this is 32-bit Raspberry Pi OS on a 64-bit kernel; the ESP32 compiler won't run.
  Fix: flash the 64-bit Raspberry Pi OS (Raspberry Pi Imager > 'Raspberry Pi OS (64-bit)').
  (Or add 'arm_64bit=0' to /boot/firmware/config.txt and reboot, so everything is 32-bit;
   that is untested with the ESP32 core.)" ;;
  armv7l/armhf) cli_arch=ARMv7
    echo "warning: 32-bit ARM is untested with the ESP32 core; 64-bit Raspberry Pi OS is recommended" ;;
  *) die "unsupported machine $machine/$userland" ;;
esac
free_gb=$(( $(df --output=avail -k "$HOME" | tail -1) / 1024 / 1024 ))
(( free_gb >= 4 )) || die "only ${free_gb} GB free in $HOME; the ESP32 core needs about 4 GB"
mem_mb=$(( $(awk '/MemTotal/ {print $2}' /proc/meminfo) / 1024 ))
(( mem_mb >= 1500 )) || echo "warning: ${mem_mb} MB RAM; builds may run out of memory (2 GB or more is best)"

step "System packages"
sudo apt-get update -qq
sudo apt-get install -y -qq git curl can-utils build-essential cmake python3-dev python3-venv >/dev/null
echo "ok"

step "arduino-cli $ARDUINO_CLI_VERSION -> $BIN"
mkdir -p "$BIN"
if ! "$BIN/arduino-cli" version 2>/dev/null | grep -q "Version: $ARDUINO_CLI_VERSION"; then
  # From GitHub releases: the install.sh one-liner fetches from downloads.arduino.cc, which has
  # failed for some Pi users.
  tmp="$(mktemp -d)"
  curl -fsSL -o "$tmp/cli.tgz" \
    "https://github.com/arduino/arduino-cli/releases/download/v$ARDUINO_CLI_VERSION/arduino-cli_${ARDUINO_CLI_VERSION}_Linux_${cli_arch}.tar.gz"
  tar -xzf "$tmp/cli.tgz" -C "$tmp" arduino-cli
  install -m 0755 "$tmp/arduino-cli" "$BIN/arduino-cli"
  rm -rf "$tmp"
fi
"$BIN/arduino-cli" version

step "ESP32 Arduino core $ESP32_CORE (about 4 GB, takes a while the first time)"
cli="$BIN/arduino-cli"
[[ -f $HOME/.arduino15/arduino-cli.yaml ]] || "$cli" config init >/dev/null
"$cli" config get board_manager.additional_urls 2>/dev/null | grep -q "$ESP32_INDEX" ||
  "$cli" config add board_manager.additional_urls "$ESP32_INDEX"
"$cli" core update-index
"$cli" core install "$ESP32_CORE"

step "Host library and tools -> $VENV (builds C++: 5-10 minutes on a Pi 4)"
# Without PYTHONPATH: on a Pi with ROS 2 sourced it points at ROS's packages, and pip would check
# (and complain about) those instead of just the venv's.
vpip() { env -u PYTHONPATH "$VENV/bin/pip" "$@"; }
[[ -x $VENV/bin/python ]] || env -u PYTHONPATH python3 -m venv "$VENV"
vpip install -q --upgrade pip
vpip install -q -e "$repo/host/python/anv_schema"   # -e: `git pull` updates it
echo "building the host library (output hidden; 5-10 minutes on a Pi 4)..."
vpip install -q "$repo/host/anv_can"                # rerun setup after a pull
for tool in anv-build anv-flash anv-host anv-can anv-schema; do
  ln -sf "$VENV/bin/$tool" "$BIN/$tool"
done
# ANV_SCHEMA_JSON: every tool and script reads the repo's schema, so a new node type works after
# `git pull` / `anv-build` without reinstalling the host library.
grep -qs 'ANV_SCHEMA_JSON=' "$HOME/.profile" || {
  printf '\n# Anveshak CAN (host/tools/pi/setup.sh)\n'
  printf 'export ANV_REPO="%s"\n' "$repo"
  printf 'export ANV_SCHEMA_JSON="%s/schema/generated/schema.json"\n' "$repo"
  # shellcheck disable=SC2016  # $PATH is meant literally: .profile expands it at login
  printf 'case ":$PATH:" in *":%s:"*) ;; *) export PATH="%s:$PATH" ;; esac\n' "$BIN" "$BIN"
} >>"$HOME/.profile"
export PATH="$BIN:$PATH" ANV_REPO="$repo" ANV_SCHEMA_JSON="$repo/schema/generated/schema.json"
echo "tools in $BIN: anv-build anv-flash anv-host anv-can anv-schema"

step "USB serial access (first flash of a new board)"
if id -nG "$USER" | grep -qw dialout; then
  echo "$USER is in dialout"
else
  sudo usermod -aG dialout "$USER"
  echo "added $USER to dialout: log out and in (or reboot) before using --usb"
fi

step "CAN interfaces at boot: $can_ifaces ($BITRATE bit/s, auto restart after bus-off)"
sudo tee /etc/systemd/system/anv-can@.service >/dev/null <<EOF
[Unit]
Description=Bring up CAN interface %i at $BITRATE bit/s
After=sys-subsystem-net-devices-%i.device
BindsTo=sys-subsystem-net-devices-%i.device

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStartPre=-/sbin/ip link set %i down
ExecStart=/sbin/ip link set %i up type can bitrate $BITRATE restart-ms 100
ExecStop=/sbin/ip link set %i down

[Install]
WantedBy=sys-subsystem-net-devices-%i.device
EOF
sudo systemctl daemon-reload
missing=()
for i in $can_ifaces; do
  sudo systemctl enable "anv-can@$i.service" >/dev/null 2>&1
  if ip link show "$i" >/dev/null 2>&1; then
    if sudo systemctl restart "anv-can@$i.service" 2>/dev/null; then
      echo "$i: up"
    else
      # Not fatal: building works without CAN. Show why, since systemd's message doesn't.
      echo "$i: FAILED to come up. Last log lines:"
      sudo journalctl -u "anv-can@$i.service" -n 4 --no-pager -o cat 2>/dev/null | sed 's/^/    /'
      echo "  'Connection timed out' / 'No such device': check this channel's interrupt= and"
      echo "  oscillator= in /boot/firmware/config.txt. 'Device or resource busy': something else"
      echo "  manages $i (systemctl list-units | grep -i can). One-channel HAT: rerun with --can can0."
    fi
  else
    missing+=("$i")
  fi
done
if (( ${#missing[@]} )); then
  cat <<EOF
not present yet: ${missing[*]}. For an MCP2515 dual CAN HAT add to /boot/firmware/config.txt
(check the crystal on the HAT: most are 16 MHz; the interrupt pins depend on the HAT), then reboot:
  dtparam=spi=on
  dtoverlay=mcp2515-can0,oscillator=16000000,interrupt=23
  dtoverlay=mcp2515-can1,oscillator=16000000,interrupt=25
They come up by themselves after the reboot.
EOF
fi

step "Check"
"$BIN/anv-build" --check
if (( warm )); then
  step "First build of the bench sketch for both chips (fills the caches: later builds are quick)"
  for chip in esp32 esp32s3; do
    "$BIN/anv-build" "$repo/firmware/nodes/bench_esp32_arduino" --chip "$chip" --no-gen | tail -2
  done
fi

cat <<EOF

Done. Open a new terminal (or: source ~/.profile), then:
  anv-build <sketch> --usb /dev/ttyUSB0 --chip esp32     first flash of a new board, over USB
  anv-build <sketch> <node> --can can1                   every later flash, over CAN
  anv-host list can1                                     what's on the bus
See host/tools/pi/README.md.
EOF
