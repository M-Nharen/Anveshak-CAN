#!/usr/bin/env python3
"""Validate defs/rover.dbc and defs/errors.yaml.

Fails (non-zero exit, one line per problem) if:
  - two messages share a CAN ID
  - a CAN ID doesn't decompose into class (0-7) / node (0-31) / sub (0-7), or is outside the
    classic 11-bit range, or the frame is extended, or the payload is over 8 bytes
  - a message with exactly one, individually-named sender has that sender's own node ID
    coded into the wrong place in its own ID
  - a message has no sender at all
  - an error code in errors.yaml is duplicated

Run after any change to defs/rover.dbc or defs/errors.yaml; CI runs it on every push.
"""
from __future__ import annotations

import sys
from pathlib import Path

import cantools
import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
DBC_PATH = REPO_ROOT / "defs" / "rover.dbc"
ERRORS_PATH = REPO_ROOT / "defs" / "errors.yaml"

# Node IDs from docs/architecture.md "Identifier scheme". A drive/FPGA node is always the
# subject of its own node field, whichever class it sends in (feedback, telemetry, debug,
# events, heartbeat, service responses, bootloader replies). A host is only the subject of
# its own node field in its own class-0 announcements (heartbeat, lead request); everywhere
# else (commands, service requests, bootloader transfers) the node field addresses the node
# being talked to, not the host doing the talking, so hosts are checked only there.
PERIPHERAL_NODE_IDS = {
    "DRIVE1": 4,
    "DRIVE2": 5,
    "DRIVE3": 6,
    "DRIVE4": 7,
    "DRIVE5": 8,
    "DRIVE6": 9,
    "FPGA": 20,
}
HOST_NODE_IDS = {
    "ORIN": 1,
    "RPI": 2,
    "TOOL": 3,
}


def check_dbc(db: cantools.database.can.Database) -> list[str]:
    problems: list[str] = []
    seen_ids: dict[int, str] = {}

    for m in db.messages:
        if m.frame_id in seen_ids:
            problems.append(
                f"duplicate CAN ID 0x{m.frame_id:03X}: {seen_ids[m.frame_id]} and {m.name}"
            )
        else:
            seen_ids[m.frame_id] = m.name

        if not (0 <= m.frame_id <= 0x7FF):
            problems.append(f"{m.name}: ID 0x{m.frame_id:X} is outside the 11-bit classic CAN range")
        if m.is_extended_frame:
            problems.append(f"{m.name}: extended-frame ID, but this bus is CAN 2.0A (11-bit) only")
        if m.length > 8:
            problems.append(f"{m.name}: length {m.length} exceeds the 8-byte classic CAN payload")

        klass = (m.frame_id >> 8) & 0x7
        node = (m.frame_id >> 3) & 0x1F
        sub = m.frame_id & 0x7
        if m.frame_id != ((klass << 8) | (node << 3) | sub):
            problems.append(f"{m.name}: ID 0x{m.frame_id:X} does not decompose into class/node/sub")

        if not m.senders:
            problems.append(f"{m.name}: has no sender")
        elif len(m.senders) == 1:
            sender = m.senders[0]
            expected = PERIPHERAL_NODE_IDS.get(sender)
            if expected is None and klass == 0:
                expected = HOST_NODE_IDS.get(sender)
            if expected is not None and node != expected:
                problems.append(
                    f"{m.name}: node field {node} in ID 0x{m.frame_id:X} does not match "
                    f"sender {sender}'s own node ID {expected}"
                )

    return problems


def check_errors(errors: list[dict]) -> list[str]:
    problems: list[str] = []
    seen_codes: dict[int, str] = {}
    seen_names: dict[str, int] = {}
    for e in errors:
        code = e["code"]
        name = e["name"]
        if code in seen_codes:
            problems.append(f"duplicate error code 0x{code:04X}: {seen_codes[code]} and {name}")
        else:
            seen_codes[code] = name
        if name in seen_names:
            problems.append(f"duplicate error name {name}: 0x{seen_names[name]:04X} and 0x{code:04X}")
        else:
            seen_names[name] = code
    return problems


def main() -> int:
    db = cantools.database.load_file(DBC_PATH)
    errors = yaml.safe_load(ERRORS_PATH.read_text())["errors"]

    problems = check_dbc(db) + check_errors(errors)

    if problems:
        for p in problems:
            print(f"FAIL: {p}", file=sys.stderr)
        print(f"\n{len(problems)} problem(s) found.", file=sys.stderr)
        return 1

    print(f"OK: {len(db.messages)} messages, {len(errors)} error codes, no problems found.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
