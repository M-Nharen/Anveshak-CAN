#!/usr/bin/env bash
# Sets up a Raspberry Pi (Pi OS Bookworm desktop, Waveshare 2-CH CAN HAT, 7" HDMI touch screen) as
# the bench host: CAN overlays, the CAN interface at 500 kbps, the gateway, the dashboard, and the
# Bench page full-screen on the display at login. Safe to run again after a `git pull` (rebuilds and
# restarts).
#
#   sudo host/tools/rpi-bench/install.sh                 everything, on can1
#   sudo host/tools/rpi-bench/install.sh --iface can0    use the HAT's other channel
#   sudo host/tools/rpi-bench/install.sh --no-overlays   leave /boot/firmware/config.txt alone
#   sudo host/tools/rpi-bench/install.sh --v1            the old 11-bit gateway instead of the v2 one
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
  echo "run with sudo: sudo $0 $*" >&2
  exit 1
fi

edit_overlays=1
v1=0  # the v2 bench gateway (host/apps/bench_gateway) unless --v1
# On this bench the bus is wired to the interface Linux calls can1 (the overlay order makes the
# names differ from the HAT's printed labels).
iface=can1
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-overlays) edit_overlays=0 ;;
    --v1) v1=1 ;;
    --iface) iface="${2:?--iface needs a name, e.g. can1}"; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
user="${SUDO_USER:-pi}"
home="$(getent passwd "$user" | cut -d: -f6)"
prefix=/opt/anveshak
reboot_needed=0

step() { printf '\n=== %s\n' "$*"; }

step "Packages (the first run downloads a few hundred MB: nodejs/npm pull in many packages)"
apt-get update
apt-get install -y build-essential cmake can-utils python3-venv python3-dev curl nodejs npm
if ! command -v chromium >/dev/null && ! command -v chromium-browser >/dev/null; then
  apt-get install -y chromium-browser || apt-get install -y chromium
fi

step "CAN HAT overlays"
config=/boot/firmware/config.txt
[[ -f $config ]] || config=/boot/config.txt
# Waveshare 2-CH CAN HAT: two MCP2515s with 16 MHz crystals, interrupts on GPIO 23 (can0) and 25 (can1).
overlays=(
  "dtparam=spi=on"
  "dtoverlay=mcp2515-can0,oscillator=16000000,interrupt=23"
  "dtoverlay=mcp2515-can1,oscillator=16000000,interrupt=25"
)
if [[ $edit_overlays -eq 1 ]]; then
  missing=()
  for line in "${overlays[@]}"; do grep -qxF "$line" "$config" || missing+=("$line"); done
  if [[ ${#missing[@]} -gt 0 ]]; then
    cp "$config" "$config.bak-anveshak"
    { echo; echo "# Anveshak CAN bench (host/tools/rpi-bench)"; printf '%s\n' "${missing[@]}"; } >>"$config"
    echo "added to $config (backup: $config.bak-anveshak):"
    printf '  %s\n' "${missing[@]}"
    reboot_needed=1
  else
    echo "already in $config"
  fi
else
  echo "skipped; make sure $config has:"
  printf '  %s\n' "${overlays[@]}"
fi

step "Python environment (the dashboard, and on v2 the gateway, run from it)"
if [[ ! -x $prefix/dashboard-venv/bin/python ]]; then
  python3 -m venv "$prefix/dashboard-venv"
fi
"$prefix/dashboard-venv/bin/pip" install --progress-bar on -r "$repo/dashboard/backend/requirements.txt" \
  -r "$repo/host/tools/anv_flash/requirements.txt"
install -d -m 0755 /etc/anveshak "$prefix/bin"

if [[ $v1 -eq 0 ]]; then
  step "Gateway: v2 libraries (anv_can builds C++ bindings: a few minutes on a Pi)"
  "$prefix/dashboard-venv/bin/pip" install --progress-bar on "$repo/host/anv_can" "$repo/host/apps/bench_gateway"
  # The v2 tools on the command line: anv-host (nodes, settings, arm, events) and anv-flash.
  ln -sf "$prefix/dashboard-venv/bin/anv-flash" /usr/local/bin/anv-flash
  ln -sf "$prefix/dashboard-venv/bin/anv-host" /usr/local/bin/anv-host
  ln -sf "$prefix/dashboard-venv/bin/anv-can" /usr/local/bin/anv-can
  gw_unit=anv-bench-gateway
  flash_mode=gateway
else
  step "Gateway (rover.c is a large generated file: a few minutes on a Pi, with no output while it compiles)"
  build="$repo/build-gw-rpi"
  sudo -u "$user" cmake -S "$repo/host/gateway" -B "$build" -DCMAKE_BUILD_TYPE=Release -DANV_GATEWAY_TESTS=OFF >/dev/null
  sudo -u "$user" cmake --build "$build" -j"$(nproc)"
  install -D -m 0755 "$build/anv_can_gateway" "$prefix/bin/anv_can_gateway"
  install -d -m 0755 /etc/anveshak
  if [[ ! -f /etc/anveshak/gateway.yaml ]]; then
    install -m 0644 "$repo/host/gateway/config/rpi-bench.yaml" /etc/anveshak/gateway.yaml
    echo "wrote /etc/anveshak/gateway.yaml"
  else
    echo "kept existing /etc/anveshak/gateway.yaml"
  fi
  sed -i "s|^interface:.*|interface: $iface|" /etc/anveshak/gateway.yaml
  echo "gateway interface: $iface"
  # anv-flash (old, 11-bit) on the command line, using the same venv the dashboard runs it from.
  cat >"$prefix/bin/anv-flash" <<EOF
#!/bin/sh
PYTHONPATH="$repo/host/tools" exec "$prefix/dashboard-venv/bin/python" -m anv_flash --channel "$iface" "\$@"
EOF
  chmod 0755 "$prefix/bin/anv-flash"
  ln -sf "$prefix/bin/anv-flash" /usr/local/bin/anv-flash
  gw_unit=anv-gateway
  flash_mode=anv_flash
fi

step "arduino-cli (the Flash button builds a user_app.cpp into firmware with it)"
arduino_cli="$(sudo -u "$user" bash -lc 'command -v arduino-cli' 2>/dev/null || true)"
[[ -z $arduino_cli && -x $home/.local/bin/arduino-cli ]] && arduino_cli="$home/.local/bin/arduino-cli"
if [[ -n $arduino_cli ]] && sudo -u "$user" "$arduino_cli" core list 2>/dev/null | grep -q '^esp32:esp32'; then
  echo "found $arduino_cli with the esp32 core"
else
  echo "arduino-cli or its esp32 core is missing: uploading a .bin still works, a .cpp needs them."
  echo "Set it up once as $user (see host/tools/rpi-bench/README.md, 'Flash from the screen'), then re-run this."
  arduino_cli="${arduino_cli:-$home/.local/bin/arduino-cli}"
fi

step "Dashboard"
(cd "$repo/dashboard/frontend" && sudo -u "$user" npm install --no-audit --no-fund && sudo -u "$user" npm run build)
if [[ ! -f /etc/anveshak/dashboard.env ]]; then
  password="$(python3 -c 'import secrets; print(secrets.token_urlsafe(8))')"
  echo "ANV_DASH_PASSWORD=$password" >/etc/anveshak/dashboard.env
  chown "$user" /etc/anveshak/dashboard.env
  chmod 0600 /etc/anveshak/dashboard.env
  echo "dashboard password (for other machines; the touch screen needs none): $password"
fi

step "Services"
# An earlier version of this script installed a can0-only unit; replace it with the template.
if [[ -f /etc/systemd/system/anv-can0.service ]]; then
  systemctl disable --now anv-can0 >/dev/null 2>&1 || true
  rm -f /etc/systemd/system/anv-can0.service
fi
fill() {
  sed -e "s|@USER@|$user|g" -e "s|@PREFIX@|$prefix|g" -e "s|@REPO@|$repo|g" -e "s|@IFACE@|$iface|g" \
    -e "s|@FLASH_MODE@|$flash_mode|g" -e "s|@ARDUINO_CLI@|$arduino_cli|g" "$here/systemd/$1.service"
}
fill anv-can@ >/etc/systemd/system/anv-can@.service
fill "$gw_unit" >/etc/systemd/system/anv-gateway.service  # same name either way: the dashboard needs it
fill anv-dashboard >/etc/systemd/system/anv-dashboard.service
systemctl daemon-reload
systemctl enable "anv-can@$iface" anv-gateway anv-dashboard >/dev/null
if ip link show "$iface" >/dev/null 2>&1; then
  systemctl restart "anv-can@$iface" anv-gateway anv-dashboard
  echo "started on $iface; logs: journalctl -fu anv-gateway"
else
  echo "$iface does not exist yet (needs the overlays and a reboot); the services start at boot"
  reboot_needed=1
fi

step "Kiosk screen"
install -m 0755 "$here/anv-kiosk.sh" "$prefix/bin/anv-kiosk.sh"
install -d -o "$user" -g "$user" "$home/.config/autostart"
sed "s|@PREFIX@|$prefix|g" "$here/anv-kiosk.desktop" >"$home/.config/autostart/anv-kiosk.desktop"
chown "$user:$user" "$home/.config/autostart/anv-kiosk.desktop"
echo "the Bench page opens full-screen when $user logs in to the desktop"

step "Done"
if [[ $reboot_needed -eq 1 ]]; then
  echo "Reboot now (sudo reboot) to load the CAN HAT overlays."
fi
echo "Check the bus with: candump -td $iface"
