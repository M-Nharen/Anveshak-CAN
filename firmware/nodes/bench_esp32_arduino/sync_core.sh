#!/usr/bin/env bash
# Copies anveshak_can_core and the generated CAN code into src/anv/, because the Arduino IDE only
# compiles files inside the sketch folder. The copy is committed so the sketch opens and builds
# straight from a checkout; CI runs `sync_core.sh --check` so it can never drift from the sources.
#
#   ./sync_core.sh           refresh src/anv/ after changing firmware/core or regenerating defs
#   ./sync_core.sh --check   exit 1 if src/anv/ is out of date
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fw="$(cd "$here/../.." && pwd)"
dest="$here/src/anv"

sources=("$fw"/core/include/*.h "$fw"/core/src/*.c "$fw"/generated/rover.c "$fw"/generated/rover.h "$fw"/generated/ids.h)

if [[ "${1:-}" == "--check" ]]; then
  stale=0
  for f in "${sources[@]}"; do
    if ! cmp -s "$f" "$dest/$(basename "$f")"; then
      echo "out of date: src/anv/$(basename "$f")" >&2
      stale=1
    fi
  done
  for f in "$dest"/*; do
    found=0
    for s in "${sources[@]}"; do [[ "$(basename "$s")" == "$(basename "$f")" ]] && found=1; done
    if [[ $found -eq 0 ]]; then
      echo "not in the sources any more: src/anv/$(basename "$f")" >&2
      stale=1
    fi
  done
  [[ $stale -eq 0 ]] || { echo "run firmware/nodes/bench_esp32_arduino/sync_core.sh" >&2; exit 1; }
  echo "src/anv is up to date"
  exit 0
fi

rm -rf "$dest"
mkdir -p "$dest"
cp "${sources[@]}" "$dest/"
echo "copied ${#sources[@]} files into src/anv/"
