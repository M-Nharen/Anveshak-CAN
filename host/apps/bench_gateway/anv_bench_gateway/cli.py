"""anv-bench-gateway: the always-running v2 host on the bench RPi.

    anv-bench-gateway --interface can1 --socket /run/anveshak/gateway.sock \\
        --id-table /var/lib/anveshak/node_ids_v2.txt
"""

from __future__ import annotations

import argparse
import signal
import sys
import threading
import time

import anv_can

from .bridge import Bridge
from .gateway import BenchGateway, GatewayConfig


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="anv-bench-gateway", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--interface", default="can1", help="SocketCAN interface (default can1)")
    p.add_argument("--host-id", type=int, default=2, help="this host's ID on the bus (default 2, the RPi)")
    p.add_argument("--tie-break", type=int, default=0, help="lower wins a same-term clash; 0 = host ID")
    p.add_argument("--socket", default="/run/anveshak/gateway.sock", help="the dashboard's socket")
    p.add_argument("--id-table", default="", help="file remembering which board (MAC) got which ID")
    p.add_argument("--id-first", type=int, default=16, help="first node ID to give out (default 16)")
    p.add_argument("--id-last", type=int, default=200, help="last node ID to give out (default 200)")
    p.add_argument("--schema", default=None, help="schema.json (default: the bundled copy)")
    p.add_argument("--status-interval", type=float, default=0, help="print a status line every N s")
    return p.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    a = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        schema = anv_can.load_schema(a.schema)
    except (FileNotFoundError, ValueError) as e:
        print(f"anv-bench-gateway: {e}", file=sys.stderr)
        return 2
    cfg = GatewayConfig(
        host_id=a.host_id, tie_break=a.tie_break, id_first=a.id_first, id_last=a.id_last, id_table=a.id_table
    )
    try:
        gw = BenchGateway(lambda: anv_can.open_socketcan(a.interface), schema, cfg)
        gw.start()
    except (OSError, ValueError, anv_can.HostError) as e:
        print(f"anv-bench-gateway: can't start on {a.interface}: {e}", file=sys.stderr)
        return 1
    bridge = Bridge(gw, a.socket)
    try:
        bridge.start()
    except OSError as e:
        print(f"anv-bench-gateway: can't listen on {a.socket}: {e}", file=sys.stderr)
        gw.stop()
        return 1
    print(f"anv-bench-gateway: host {a.host_id} on {a.interface}, dashboard socket {a.socket}", flush=True)

    done = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: done.set())
    signal.signal(signal.SIGINT, lambda *_: done.set())
    last = time.monotonic()
    while not done.wait(0.5):
        if a.status_interval and time.monotonic() - last >= a.status_interval:
            last = time.monotonic()
            st = gw.state()
            print(
                f"{st['role_name'].lower()} term {st['term']}, leader {st['believed_master'] or '-'}, "
                f"{st['online_count']} node(s) online, {st['bus']['rx_fps']} frames/s",
                flush=True,
            )
    bridge.stop()
    gw.stop()
    return 0
