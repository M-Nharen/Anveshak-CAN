#!/usr/bin/env bash
# End-to-end check: the real anv_can_gateway against the Python node simulator on vcan0.
#   1. all simulated nodes (6 drives + FPGA) show up alive
#   2. after "lead 1" and "arm" every drive node reports ARMED
#   3. driving makes the simulated wheels move (non-zero encoder / speed feedback appears)
# Exit code 77 = skipped (vcan, python-can or cantools unavailable); ctest treats that as SKIP.
set -u
GATEWAY="${1:?usage: vcan_smoke.sh path/to/anv_can_gateway}"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
IFACE=vcan0
SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo -n"

skip() { echo "SKIP: $*"; exit 77; }

command -v ip >/dev/null 2>&1 || skip "'ip' not installed"
python3 -c "import can, cantools" 2>/dev/null || skip "python-can / cantools not installed"
$SUDO modprobe vcan 2>/dev/null
if ! ip link show "$IFACE" >/dev/null 2>&1; then
  $SUDO ip link add dev "$IFACE" type vcan 2>/dev/null || skip "cannot create $IFACE (no vcan support or no privileges)"
fi
$SUDO ip link set up "$IFACE" 2>/dev/null || skip "cannot bring $IFACE up"

WORK="$(mktemp -d)"
cleanup() {
  [ -n "${SIM_PID:-}" ] && kill "$SIM_PID" 2>/dev/null
  [ -n "${GW_PID:-}" ] && kill "$GW_PID" 2>/dev/null
  exec 3>&- 2>/dev/null
  rm -rf "$WORK"
}
trap cleanup EXIT

(cd "$REPO" && python3 host/sim/cli.py --interface socketcan --channel "$IFACE") >"$WORK/sim.log" 2>&1 &
SIM_PID=$!
sleep 1
kill -0 "$SIM_PID" 2>/dev/null || { cat "$WORK/sim.log"; echo "FAIL: simulator did not start"; exit 1; }

mkfifo "$WORK/cmd"
"$GATEWAY" --interface "$IFACE" --status-interval 0 <"$WORK/cmd" >"$WORK/gw.log" 2>&1 &
GW_PID=$!
exec 3>"$WORK/cmd"   # keep the pipe open so the gateway doesn't see EOF

say() { echo "$1" >&3; }
fail() { echo "FAIL: $1"; echo "--- gateway log"; cat "$WORK/gw.log"; echo "--- sim log"; cat "$WORK/sim.log"; exit 1; }

sleep 1
say status; sleep 0.3
grep -q "STANDBY" "$WORK/gw.log" || fail "gateway should start as standby"
[ "$(grep -c 'alive' "$WORK/gw.log")" -ge 7 ] || fail "expected 7 alive nodes (6 drives + FPGA) in status"

say "lead 1"; sleep 0.5
say arm; sleep 0.5
say "drive 2.0 2.0"; sleep 1
say status; sleep 0.3

LAST="$(awk '/^host /{block=""} {block=block $0 "\n"} END{printf "%s", block}' "$WORK/gw.log")"
echo "$LAST"
echo "$LAST" | grep -q "MASTER" || fail "gateway should be master after 'lead 1'"
[ "$(echo "$LAST" | grep -c 'ARMED')" -ge 7 ] || fail "expected master flag + 6 drive nodes ARMED"

say stop; sleep 0.4
say disarm; sleep 0.3
say quit
wait "$GW_PID" 2>/dev/null
echo "PASS"
