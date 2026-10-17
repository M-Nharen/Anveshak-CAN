#!/usr/bin/env bash
# Sets up a Raspberry Pi (Pi OS Bookworm desktop, Waveshare 2-CH CAN HAT, 7" HDMI touch screen) as
# the bench host: CAN overlays, can0 at 500 kbps, the gateway, the dashboard, and the Bench page
# full-screen on the display at login. Safe to run again after a `git pull` (rebuilds and restarts).
#
#   sudo host/tools/rpi-bench/install.sh                 everything
#   sudo host/tools/rpi-bench/install.sh --no-overlays   leave /boot/firmware/config.txt alone
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
  echo "run with sudo: sudo $0 $*" >&2
  exit 1
fi

edit_overlays=1
for arg in "$@"; do
  case "$arg" in
    --no-overlays) edit_overlays=0 ;;
    *) echo "unknown option $arg" >&2; exit 2 ;;
  esac
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
apt-get install -y build-essential cmake can-utils python3-venv curl nodejs npm
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

step "Dashboard"
if [[ ! -x $prefix/dashboard-venv/bin/python ]]; then
  python3 -m venv "$prefix/dashboard-venv"
fi
"$prefix/dashboard-venv/bin/pip" install --progress-bar on -r "$repo/dashboard/backend/requirements.txt"
(cd "$repo/dashboard/frontend" && sudo -u "$user" npm install --no-audit --no-fund && sudo -u "$user" npm run build)
if [[ ! -f /etc/anveshak/dashboard.env ]]; then
  password="$(python3 -c 'import secrets; print(secrets.token_urlsafe(8))')"
  echo "ANV_DASH_PASSWORD=$password" >/etc/anveshak/dashboard.env
  chown "$user" /etc/anveshak/dashboard.env
  chmod 0600 /etc/anveshak/dashboard.env
  echo "dashboard password (for other machines; the touch screen needs none): $password"
fi

step "Services"
for unit in anv-can0 anv-gateway anv-dashboard; do
  sed -e "s|@USER@|$user|g" -e "s|@PREFIX@|$prefix|g" -e "s|@REPO@|$repo|g" \
    "$here/systemd/$unit.service" >"/etc/systemd/system/$unit.service"
done
systemctl daemon-reload
systemctl enable anv-can0 anv-gateway anv-dashboard >/dev/null
if ip link show can0 >/dev/null 2>&1; then
  systemctl restart anv-can0 anv-gateway anv-dashboard
  echo "started; logs: journalctl -fu anv-gateway"
else
  echo "can0 does not exist yet (needs the overlays and a reboot); the services start at boot"
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
echo "Check the bus with: candump -td can0"
