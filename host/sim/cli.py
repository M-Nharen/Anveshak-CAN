#!/usr/bin/env python3
"""Run the rover simulator as a standalone process.

    python -m host.sim.cli
    python -m host.sim.cli --drop-node 6 --overcurrent 4
    python -m host.sim.cli --interface socketcan --channel vcan0
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import can

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rover_sim import Simulator, load_db


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interface", default="virtual",
                         help="python-can interface/bustype (default: virtual)")
    parser.add_argument("--channel", default="anveshak-sim",
                         help="bus channel name, e.g. vcan0 for socketcan (default: anveshak-sim)")
    parser.add_argument("--bitrate", type=int, default=500000)
    parser.add_argument("--drop-node", type=int, action="append", default=[], metavar="N",
                         help="simulate drive node N (1-6) being offline; may repeat")
    parser.add_argument("--overcurrent", type=int, action="append", default=[], metavar="N",
                         help="inject an overcurrent fault on drive node N (1-6); may repeat")
    parser.add_argument("--duration", type=float, default=None,
                         help="stop after this many seconds (default: run until interrupted)")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)

    bus_kwargs = {"interface": args.interface, "channel": args.channel}
    if args.interface != "virtual":
        bus_kwargs["bitrate"] = args.bitrate
    bus = can.interface.Bus(**bus_kwargs)

    sim = Simulator(bus, load_db())
    for n in args.drop_node:
        sim.inject_drop(n)
    for n in args.overcurrent:
        sim.inject_overcurrent(n)

    try:
        if args.duration is not None:
            sim.run_for(args.duration)
        else:
            sim.run_forever()
    except KeyboardInterrupt:
        pass
    finally:
        bus.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
