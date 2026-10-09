"""anv-flash: reflash rover nodes over CAN (docs/architecture.md, "Host flashing tool").

    anv-flash list                            # all nodes: ID, type, state, firmware build
    anv-flash flash --node 4 drive.bin        # one node
    anv-flash flash --type drive drive.bin    # every drive node, one at a time
    anv-flash rescue --uid 0x1234ABCD app.bin # a board stuck in the bootloader or on ID 31
"""

from __future__ import annotations

import argparse
import os
import sys
from collections.abc import Sequence

import can

from .bus import HOST_IDS, load_db
from .flasher import NODE_TYPE_NAMES, STATE_NAMES, Flasher, FlashError, SafetyRefusal
from .image import FirmwareImage, ImageError

EXIT_OK = 0
EXIT_FAILED = 1
EXIT_REFUSED = 2

_TYPE_IDS = {name: num for num, name in NODE_TYPE_NAMES.items()}


def _node_type(text: str) -> int:
    if text.lower() in _TYPE_IDS:
        return _TYPE_IDS[text.lower()]
    try:
        return int(text, 0)
    except ValueError:
        raise argparse.ArgumentTypeError(f"unknown node type {text!r}: one of {', '.join(_TYPE_IDS)}") from None


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="anv-flash", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--interface", default=os.environ.get("ANV_CAN_INTERFACE", "socketcan"),
                        help="python-can interface (default: socketcan, or $ANV_CAN_INTERFACE)")
    parser.add_argument("--channel", default=os.environ.get("ANV_CAN_CHANNEL", "can0"),
                        help="CAN channel (default: can0, or $ANV_CAN_CHANNEL)")
    parser.add_argument("--bitrate", type=int, default=500000)
    parser.add_argument("--host", choices=sorted(HOST_IDS), default=os.environ.get("ANV_HOST"),
                        help="which host this is (must be the master to flash); default $ANV_HOST")
    parser.add_argument("--dbc", default=None, help="path to rover.dbc (default: the repo's defs/rover.dbc)")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("list", help="list nodes: ID, type, state, firmware build")

    p_flash = sub.add_parser("flash", help="flash one node, or every node of a type")
    target = p_flash.add_mutually_exclusive_group(required=True)
    target.add_argument("--node", type=int, help="node ID")
    target.add_argument("--type", type=_node_type, help="node type: drive, arm, science, fpga, power")
    p_flash.add_argument("image", help="stamped image (.bin) from the firmware build")

    p_rescue = sub.add_parser("rescue", help="flash a board by its UID (stuck in the bootloader or on ID 31)")
    p_rescue.add_argument("--uid", required=True, type=lambda s: int(s, 0), help="32-bit board UID, e.g. 0x1234ABCD")
    p_rescue.add_argument("--timeout", type=float, default=60.0, help="seconds to wait for the board")
    p_rescue.add_argument("image")
    return parser


def _print_list(flasher: Flasher) -> None:
    rows = flasher.list_nodes()
    if not rows:
        print("no nodes heard")
        return
    print(f"{'ID':>3}  {'type':<8} {'state':<11} {'build':<6}  bootloader")
    for row in rows:
        n = row.status
        extra = ""
        if row.info is not None:
            app = "valid app" if row.info.boot_status == 0 else "no valid app"
            extra = f"uid 0x{row.info.uid32:08X}, {app}"
        print(f"{n.node_id:>3}  {NODE_TYPE_NAMES.get(n.node_type, str(n.node_type)):<8} "
              f"{STATE_NAMES.get(n.state, str(n.state)):<11} 0x{n.build_id16:04X}  {extra}")


def main(argv: Sequence[str] | None = None, bus: can.BusABC | None = None) -> int:
    args = build_parser().parse_args(argv)
    image = None
    if args.command in ("flash", "rescue"):
        try:
            image = FirmwareImage.load(args.image)
        except (OSError, ImageError) as e:
            print(f"anv-flash: {args.image}: {e}", file=sys.stderr)
            return EXIT_FAILED
        h = image.header
        print(f"image: {NODE_TYPE_NAMES.get(h.node_type, h.node_type)} firmware {h.version_str}, "
              f"{h.image_length} B, build 0x{h.build_id:08X} ({h.git_hash})")

    own_bus = bus is None
    if bus is None:
        bus = can.Bus(interface=args.interface, channel=args.channel, bitrate=args.bitrate)
    host_id = HOST_IDS.get(args.host) if args.host else None
    try:
        with Flasher(bus, load_db(args.dbc), host_id, progress=print) as flasher:
            if args.command == "list":
                _print_list(flasher)
            elif args.command == "flash":
                assert image is not None
                if args.node is not None:
                    flasher.flash_node(args.node, image)
                else:
                    flasher.flash_type(args.type, image)
            else:
                assert image is not None
                flasher.cfg.rescue_timeout_s = args.timeout
                flasher.rescue(args.uid, image)
    except SafetyRefusal as e:
        print(f"anv-flash: refused: {e}", file=sys.stderr)
        return EXIT_REFUSED
    except FlashError as e:
        print(f"anv-flash: {e}", file=sys.stderr)
        return EXIT_FAILED
    finally:
        if own_bus:
            bus.shutdown()
    if args.command != "list":
        print("done")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
