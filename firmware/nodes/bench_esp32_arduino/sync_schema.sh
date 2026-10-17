#!/usr/bin/env bash
# Copies this node type's generated code (schema/generated/c) into src/schema/, because the Arduino
# IDE only compiles files inside the sketch folder. The libraries (firmware/lib) use the generated
# protocol code in place; only the type's own files and the headers they include are copied. The
# copy is committed so the sketch builds straight from a checkout; CI runs `sync_schema.sh --check`.
#
#   ./sync_schema.sh           refresh src/schema/ after `anv-schema gen`
#   ./sync_schema.sh --check   exit 1 if src/schema/ is out of date
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
gen="$(cd "$here/../../../schema/generated/c" && pwd)"
dest="$here/src/schema"
type=esp32_bench

files=(anv_schema.h anv_protocol.h "anv_$type.h" "anv_$type.c")

if [[ "${1:-}" == "--check" ]]; then
  stale=0
  for f in "${files[@]}"; do
    if ! cmp -s "$gen/$f" "$dest/$f"; then
      echo "out of date: src/schema/$f" >&2
      stale=1
    fi
  done
  for f in "$dest"/*; do
    found=0
    for s in "${files[@]}"; do [[ "$s" == "$(basename "$f")" ]] && found=1; done
    if [[ $found -eq 0 ]]; then
      echo "not generated any more: src/schema/$(basename "$f")" >&2
      stale=1
    fi
  done
  [[ $stale -eq 0 ]] || { echo "run firmware/nodes/bench_esp32_arduino/sync_schema.sh" >&2; exit 1; }
  echo "src/schema is up to date"
  exit 0
fi

rm -rf "$dest"
mkdir -p "$dest"
for f in "${files[@]}"; do cp "$gen/$f" "$dest/"; done
echo "copied ${#files[@]} files into src/schema/"
