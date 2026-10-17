"""anv-host: see and drive the nodes on the bus, as a host would.

    anv-host list vcan0                          # nodes (and other hosts) heard
    anv-host get vcan0 20                        # every setting of node 20
    anv-host get vcan0 20 kp_0 heartbeat_hz
    anv-host set vcan0 20 kp_0=12.5 --save
    anv-host rate vcan0 20                       # every stream's rate
    anv-host rate vcan0 20 feedback 10           # Hz; 0 = off
    anv-host action vcan0 20 blink 5
    anv-host arm vcan0 20 --take-lead            # holds the lead (and the arm) until Ctrl-C
    anv-host command vcan0 20 setpoint mode=velocity "target=[1,1]" --take-lead --arm --duration 2
    anv-host disarm vcan0 all --take-lead
    anv-host estop vcan0                         # anyone, any time
    anv-host estop vcan0 --release --take-lead
    anv-host lead vcan0 2                        # ask host 2 to lead
    anv-host debug vcan0 20 2 --take-lead         # debug level 2 on node 20
    anv-host debug vcan0 all 0 --take-lead        # back to off, every node
    anv-host events vcan0                        # watch error events as they happen

The tool joins the bus as a host with ID --host-id (default $ANV_HOST_ID, else 250). Arming,
commands and E-stop release need the lead; --take-lead asks for it, which (like any change of
leader) disarms every node first. When the tool exits its heartbeat stops, so nodes it armed
disarm on their own (leader timeout); it also disarms them on the way out.
"""
from __future__ import annotations

import argparse
import os
import sys
import time
from collections.abc import Callable
from typing import Any, TextIO

from . import Host, HostError, Link, Schema, enum_label, load_schema
from .cli import parse_value


def _node_arg(text: str) -> int:
    return 0 if text == "all" else int(text, 0)


def _open(open_link: Callable[[str], Link] | None, name: str) -> Link:
    if open_link is not None:
        return open_link(name)
    from . import open_socketcan
    return open_socketcan(name)


def _mac(mac: int) -> str:
    return ":".join(f"{(mac >> s) & 0xFF:02x}" for s in range(40, -8, -8)) if mac else "-"


def cmd_list(host: Host, schema: Schema, a: argparse.Namespace, out: TextIO) -> int:
    time.sleep(a.wait)
    nodes = sorted(host.nodes(), key=lambda n: n.id)
    print(f"{'ID':>3}  {'TYPE':<20} {'STATE':<10} {'SCHEMA':<16} {'HB Hz':>6} {'UPTIME':>7}  MAC",
          file=out)
    for n in nodes:
        tname = n.type.name if n.type is not None else f"type {n.type_id}"
        state = enum_label(schema, "node_state", n.state) if n.online else "offline"
        flags = (" fault" if n.fault_active else "") + (" unsaved" if n.unsaved_params else "")
        print(f"{n.id:>3}  {tname:<20} {state:<10} {n.compat:<16} {n.heartbeat_hz:>6.1f} "
              f"{n.uptime_s:>6}s  {_mac(n.mac)}{flags}", file=out)
    for h in sorted(host.hosts(), key=lambda h: h.id):
        if h.online:
            print(f"{h.id:>3}  {'(host)':<20} {h.role:<10} term {h.term}", file=out)
    if not nodes:
        print("no nodes heard", file=out)
    return 0


def _wait_node(host: Host, node: int, wait: float) -> None:
    if not host.wait_for_node(node, int(wait * 1000)):
        raise HostError(f"node {node} not heard within {wait:g} s")
    info = host.node(node)
    if not info.usable:
        raise HostError(f"node {node}: {info.compat} (type_id {info.type_id}); "
                        "refusing to talk to it")


def _fmt(v: float) -> str:
    return f"{v:.7g}"


def cmd_get(host: Host, schema: Schema, a: argparse.Namespace, out: TextIO) -> int:
    _wait_node(host, a.node, a.wait)
    names = a.params or [p.name for p in host.node(a.node).type.params] + [
        p.name for p in schema.protocol.params]
    bad = 0
    for name in names:
        p = host.find_param(a.node, name)
        try:
            v = host.get(a.node, name)
            unit = f" {p.unit}" if p is not None and p.unit else ""
            print(f"{name} = {_fmt(v)}{unit}", file=out)
        except HostError as e:
            print(f"{name}: {e}", file=out)
            bad += 1
    return 1 if bad else 0


def cmd_set(host: Host, schema: Schema, a: argparse.Namespace, out: TextIO) -> int:
    _wait_node(host, a.node, a.wait)
    for item in a.assignments:
        name, sep, text = item.partition("=")
        if not sep:
            raise ValueError(f"expected PARAM=VALUE, got {item!r}")
        value = parse_value(text)
        if not isinstance(value, (bool, int, float)):
            raise ValueError(f"{name}: {text!r} is not a number")
        print(f"{name} = {_fmt(host.set(a.node, name, float(value)))}", file=out)
    if a.save:
        host.save(a.node)
        print("saved", file=out)
    return 0


def cmd_save(host: Host, schema: Schema, a: argparse.Namespace, out: TextIO) -> int:
    _wait_node(host, a.node, a.wait)
    host.save(a.node)
    print("saved", file=out)
    return 0


def cmd_rate(host: Host, schema: Schema, a: argparse.Namespace, out: TextIO) -> int:
    _wait_node(host, a.node, a.wait)
    if a.stream is None:
        for m in host.node(a.node).type.messages:
            if m.kind == "stream":
                print(f"{m.name} = {_fmt(host.get_rate(a.node, m.name))} Hz", file=out)
        return 0
    if a.hz is None:
        print(f"{a.stream} = {_fmt(host.get_rate(a.node, a.stream))} Hz", file=out)
    else:
        print(f"{a.stream} = {_fmt(host.set_rate(a.node, a.stream, a.hz))} Hz", file=out)
    return 0


def cmd_action(host: Host, schema: Schema, a: argparse.Namespace, out: TextIO) -> int:
    _wait_node(host, a.node, a.wait)
    host.action(a.node, a.name, a.arg)
    print("done", file=out)
    return 0


def _lead(host: Host, a: argparse.Namespace) -> None:
    time.sleep(0.3)  # hear the other hosts first, so a claim beats the current term
    if host.leading:
        return
    if not a.take_lead:
        who = f"host {host.leader} leads" if host.leader else "nobody leads"
        raise HostError(f"not the leader ({who}); pass --take-lead")
    host.request_lead()
    if not host.wait_for_lead(1000):
        raise HostError("couldn't take the lead")
    # wait_for_lead() only reflects this host's own election state; give the claim a moment to
    # actually reach the nodes before sending them anything that depends on it (arm, debug
    # level, ...). A one-shot send right after taking the lead would otherwise race ahead of
    # the nodes' own view of who leads and be silently ignored.
    time.sleep(0.1)


def _hold(host: Host, seconds: float) -> None:
    end = time.monotonic() + seconds if seconds > 0 else None
    try:
        while (end is None or time.monotonic() < end) and host.leading:
            time.sleep(0.05)
    except KeyboardInterrupt:
        pass


def cmd_arm(host: Host, schema: Schema, a: argparse.Namespace, out: TextIO) -> int:
    if a.node:
        _wait_node(host, a.node, a.wait)
    _lead(host, a)
    host.arm(a.node)
    print(f"armed {a.node or 'all'} (term {host.term}); holding the lead"
          f"{f' for {a.hold:g} s' if a.hold else ', Ctrl-C to stop'}", file=out, flush=True)
    _hold(host, a.hold)
    if host.leading:
        host.disarm(a.node)
    return 0


def cmd_disarm(host: Host, schema: Schema, a: argparse.Namespace, out: TextIO) -> int:
    _lead(host, a)
    host.disarm(a.node)
    print(f"disarmed {a.node or 'all'}", file=out)
    return 0


def cmd_command(host: Host, schema: Schema, a: argparse.Namespace, out: TextIO) -> int:
    _wait_node(host, a.node, a.wait)
    values: dict[str, Any] = {}
    for item in a.fields:
        name, sep, text = item.partition("=")
        if not sep:
            raise ValueError(f"expected field=value, got {item!r}")
        values[name] = parse_value(text)
    _lead(host, a)
    if a.arm:
        host.arm(a.node)
    host.set_command(a.node, a.message, values, a.hz)
    print(f"sending {a.message} to {a.node} at {a.hz or a.command_hz:g} Hz", file=out, flush=True)
    _hold(host, a.duration)
    host.stop_command(a.node, a.message)
    if host.leading:
        host.disarm(a.node)
    return 0


def cmd_estop(host: Host, schema: Schema, a: argparse.Namespace, out: TextIO) -> int:
    if a.release:
        _lead(host, a)
        host.estop_release()
        print("E-stop released", file=out)
    else:
        host.estop(a.reason)
        print("E-stop sent", file=out)
    return 0


def cmd_debug(host: Host, schema: Schema, a: argparse.Namespace, out: TextIO) -> int:
    if a.node:
        _wait_node(host, a.node, a.wait)
    _lead(host, a)
    host.set_debug_level(a.node, a.level, categories=a.categories, timeout_s=a.timeout)
    until = f" for {a.timeout} s" if a.timeout else " until changed"
    print(f"debug level {a.level} on {a.node or 'all'}{until}", file=out)
    return 0


def _print_event(e: dict[str, Any], out: TextIO) -> None:
    cleared = " (cleared)" if e["cleared"] else ""
    name = e["name"] or f"0x{e['code']:04x}"
    print(f"node {e['node']:>3}  {e['severity']:<8} {name}{cleared}  detail={e['detail']} "
          f"uptime={e['uptime_s']}s", file=out, flush=True)


def cmd_events(host: Host, schema: Schema, a: argparse.Namespace, out: TextIO) -> int:
    # set_callbacks() must run before start(): main() has already wired this one in.
    print(f"watching for events{f' for {a.duration:g} s' if a.duration else ', Ctrl-C to stop'}",
          file=out, flush=True)
    end = time.monotonic() + a.duration if a.duration > 0 else None
    try:
        while end is None or time.monotonic() < end:
            time.sleep(0.05)
    except KeyboardInterrupt:
        pass
    return 0


def cmd_lead(host: Host, schema: Schema, a: argparse.Namespace, out: TextIO) -> int:
    time.sleep(0.3)
    host.request_lead(a.host)
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline and host.leader != a.host:
        time.sleep(0.02)
    if host.leader != a.host:
        print(f"asked host {a.host} to lead; it hasn't claimed yet", file=out)
        return 1
    print(f"host {a.host} leads (term {host.term})", file=out)
    return 0


def _parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="anv-host", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--schema", help="schema.json (default: $ANV_SCHEMA_JSON or the bundled copy)")
    ap.add_argument("--host-id", type=lambda x: int(x, 0),
                    default=int(os.environ.get("ANV_HOST_ID", "250"), 0),
                    help="this tool's node ID on the bus (default $ANV_HOST_ID or 250)")
    ap.add_argument("--wait", type=float, default=2.0,
                    help="seconds to wait for nodes' heartbeats (default 2)")
    ap.add_argument("--command-hz", type=float, default=50.0,
                    help="command refresh rate (default 50)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def p(name: str, fn: Callable[..., int], help: str) -> argparse.ArgumentParser:
        sp = sub.add_parser(name, help=help)
        sp.set_defaults(fn=fn)
        sp.add_argument("interface")
        return sp

    p("list", cmd_list, "nodes and hosts on the bus")
    sp = p("get", cmd_get, "read settings")
    sp.add_argument("node", type=_node_arg)
    sp.add_argument("params", nargs="*", help="names or indices (default: all)")
    sp = p("set", cmd_set, "change settings")
    sp.add_argument("node", type=_node_arg)
    sp.add_argument("assignments", nargs="+", metavar="PARAM=VALUE")
    sp.add_argument("--save", action="store_true", help="also save them to the node's flash")
    sp = p("save", cmd_save, "save a node's settings to its flash")
    sp.add_argument("node", type=_node_arg)
    sp = p("rate", cmd_rate, "read or set stream rates")
    sp.add_argument("node", type=_node_arg)
    sp.add_argument("stream", nargs="?")
    sp.add_argument("hz", nargs="?", type=float)
    sp = p("action", cmd_action, "run an action")
    sp.add_argument("node", type=_node_arg)
    sp.add_argument("name")
    sp.add_argument("arg", nargs="?", type=float)
    for name, fn, help in (("arm", cmd_arm, "arm a node (or all)"),
                           ("disarm", cmd_disarm, "disarm a node (or all)")):
        sp = p(name, fn, help)
        sp.add_argument("node", type=_node_arg, help="node ID or 'all'")
        sp.add_argument("--take-lead", action="store_true", help="become the leader first")
        if name == "arm":
            sp.add_argument("--hold", type=float, default=0,
                            help="seconds to hold (default: until Ctrl-C)")
    sp = p("command", cmd_command, "send a command repeatedly")
    sp.add_argument("node", type=_node_arg)
    sp.add_argument("message")
    sp.add_argument("fields", nargs="*", metavar="field=value")
    sp.add_argument("--hz", type=float, default=0, help="refresh rate (default --command-hz)")
    sp.add_argument("--duration", type=float, default=0, help="seconds (default: until Ctrl-C)")
    sp.add_argument("--arm", action="store_true", help="arm the node first")
    sp.add_argument("--take-lead", action="store_true", help="become the leader first")
    sp = p("estop", cmd_estop, "E-stop every node (or release it)")
    sp.add_argument("--reason", type=int, default=0)
    sp.add_argument("--release", action="store_true", help="release the latch (needs the lead)")
    sp.add_argument("--take-lead", action="store_true",
                    help="become the leader first (with --release)")
    sp = p("lead", cmd_lead, "ask a host to lead")
    sp.add_argument("host", type=lambda x: int(x, 0))
    sp = p("debug", cmd_debug, "turn debugging on a node (or all) up or down")
    sp.add_argument("node", type=_node_arg, help="node ID or 'all'")
    sp.add_argument("level", type=int, help="0-3")
    sp.add_argument("--categories", type=lambda x: int(x, 0), default=0,
                    help="bitmask the node type defines (default: 0, all)")
    sp.add_argument("--timeout", type=int, default=0, metavar="SECONDS",
                    help="revert to the saved default after this long (default: until changed)")
    sp.add_argument("--take-lead", action="store_true", help="become the leader first")
    sp = p("events", cmd_events, "watch error events from every node as they happen")
    sp.add_argument("--duration", type=float, default=0, help="seconds (default: until Ctrl-C)")
    return ap


def main(argv: list[str] | None = None, open_link: Callable[[str], Link] | None = None,
         out: TextIO | None = None) -> int:
    a = _parser().parse_args(argv)
    out = out or sys.stdout
    try:
        schema = load_schema(a.schema)
    except (FileNotFoundError, ValueError) as e:
        print(f"anv-host: {e}", file=sys.stderr)
        return 2
    try:
        link = _open(open_link, a.interface)
    except ValueError as e:
        print(f"anv-host: {e}", file=sys.stderr)
        return 2
    # A tool never hands out IDs: that's the job of the hosts with the rover's ID table.
    host = Host(link, schema, host_id=a.host_id, allocator="off", command_hz=a.command_hz)
    if a.cmd == "events":  # set_callbacks() only works before start()
        host.set_callbacks(event=lambda e: _print_event(e, out))
    try:
        host.start()
        return a.fn(host, schema, a, out)
    except HostError as e:
        print(f"anv-host: {e}", file=sys.stderr)
        return 1
    except (KeyError, ValueError) as e:
        print(f"anv-host: {e}", file=sys.stderr)
        return 2
    finally:
        host.stop()


if __name__ == "__main__":
    sys.exit(main())
