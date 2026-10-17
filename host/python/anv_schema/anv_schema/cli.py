"""anv-schema: check, generate and explore the CAN schema.

    anv-schema check                  validate every file under schema/
    anv-schema gen                    write schema/generated/ (C code + schema.json)
    anv-schema gen --check            fail if schema/generated/ is out of date (CI)
    anv-schema show [TYPE]            list types, or one type's messages/params/actions/events
    anv-schema id 0x0C018610          split an ID into priority/dst/port/src (and name the port)
    anv-schema decode ID BYTES [--type T]
    anv-schema encode TYPE MSG field=value ... [--src N] [--dst N]
    anv-schema dbc BUS.yaml [-o out.dbc]
"""
from __future__ import annotations

import argparse
import ast
import sys
from pathlib import Path

from . import bundle, codec, gen_c, gen_dbc
from .loader import SchemaError, default_schema_dir, load
from .model import NodeType, Schema
from .validate import validate


def _schema(args: argparse.Namespace) -> Schema:
    return load(args.dir)


def _root(args: argparse.Namespace) -> Path:
    return Path(args.dir) if args.dir else default_schema_dir()


def generated_files(schema: Schema) -> dict[str, str]:
    """Relative path under schema/generated/ -> contents."""
    files = {f"c/{name}": text for name, text in gen_c.generate(schema).items()}
    files["schema.json"] = bundle.dumps(schema)
    return files


def cmd_check(args: argparse.Namespace) -> int:
    schema = _schema(args)
    problems = validate(schema)
    for p in problems:
        print(p, file=sys.stderr)
    if problems:
        print(f"{len(problems)} problem(s)", file=sys.stderr)
        return 1
    n_msgs = len(schema.protocol.messages) + sum(len(t.messages) for t in schema.types.values())
    print(f"OK: protocol + {len(schema.types)} node type(s), {n_msgs} messages")
    return 0


def cmd_gen(args: argparse.Namespace) -> int:
    schema = _schema(args)
    problems = validate(schema)
    if problems:
        for p in problems:
            print(p, file=sys.stderr)
        return 1
    out = Path(args.out) if args.out else _root(args) / "generated"
    files = generated_files(schema)
    stale = []
    for rel, text in files.items():
        path = out / rel
        old = path.read_text(encoding="utf-8") if path.is_file() else None
        if old != text:
            stale.append(rel)
            if not args.check:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8")
    # Files for types that no longer exist.
    if out.is_dir():
        for path in out.rglob("*"):
            rel = path.relative_to(out).as_posix()
            if path.is_file() and rel not in files:
                stale.append(rel + " (no longer generated)")
                if not args.check:
                    path.unlink()
    if args.check:
        if stale:
            print("schema/generated is out of date; run `anv-schema gen`:\n  "
                  + "\n  ".join(stale), file=sys.stderr)
            return 1
        print("schema/generated is up to date")
        return 0
    print(f"wrote {len(stale)} file(s) in {out}" if stale else f"{out} already up to date")
    return 0


def _rate(m) -> str:
    if not m.rate:
        return ""
    r = m.rate
    return f"{r.default:g} Hz ({r.min:g}-{r.max:g})"


def _show_type(schema: Schema, t: NodeType) -> None:
    print(f"{t.name}  type_id {t.type_id}  version {t.version}  wire hash {t.wire_hash:#010x}")
    if t.description:
        print(f"  {t.description}")
    print("\nmessages")
    for m in sorted(t.messages, key=lambda m: m.port):
        extra = _rate(m) or (f"watchdog {m.watchdog_ms} ms" if m.watchdog_ms else "")
        if m.apply == "sync":
            extra += ", applied at SYNC"
        print(f"  port {m.port:4}  prio {m.priority}  {m.kind:7} from {m.sender:6} "
              f"{m.name:20} {m.length} B  {extra}")
        for f in m.fields:
            arr = f"[{f.count}]" if f.count > 1 else ""
            scale = ""
            if f.is_scaled:
                scale = f" x{f.scale:g}" + (f" {f.offset:+g}" if f.offset else "")
            what = f" enum {f.enum.name}" if f.enum else ""
            print(f"      bit {f.start_bit:2}+{f.bits:<2} {f.name}{arr}: {f.type.name}{scale}"
                  f"{' ' + f.unit if f.unit else ''}{what}")
    params = schema.all_params(t) if t is not schema.protocol else t.params
    if params:
        print("\nparameters" + (" (protocol ones first)" if t is not schema.protocol else ""))
        for p in params:
            lim = f"{p.min:g}..{p.max:g}" if p.min is not None and p.max is not None else ""
            flags = ",".join(x for x, on in (("ro", p.access == "ro"),
                                             ("disarmed", p.disarmed_only),
                                             ("no-save", not p.persist)) if on)
            print(f"  {p.index:#06x}  {p.name:24} {p.type.name:4} default {p.default:g} "
                  f"{lim} {p.unit} {flags}")
    if t.actions:
        print("\nactions")
        for a in t.actions:
            print(f"  {a.index:4}  {a.name:24} arg {a.arg.name if a.arg else '-'}"
                  f"{'  (disarmed only)' if a.disarmed_only else ''}")
    if t.events:
        print("\nevents")
        for e in t.events:
            print(f"  {e.code:#06x}  {e.severity:8} {e.name}: {e.description}")


def cmd_show(args: argparse.Namespace) -> int:
    schema = _schema(args)
    if not args.type:
        print("protocol (type 0): ports 0.." f"{schema.protocol_last_port}")
        for t in sorted(schema.types.values(), key=lambda t: t.type_id):
            print(f"{t.name:24} type {t.type_id:<5} v{t.version}  {len(t.messages)} msgs, "
                  f"{len(t.params)} params, {len(t.actions)} actions  {t.description}")
        return 0
    t = schema.protocol if args.type == "protocol" else schema.types.get(args.type)
    if t is None:
        print(f"no type {args.type!r}; try `anv-schema show`", file=sys.stderr)
        return 1
    _show_type(schema, t)
    return 0


def _int(text: str) -> int:
    return int(text, 0)


def cmd_id(args: argparse.Namespace) -> int:
    schema = _schema(args)
    parts = schema.id_layout.parse(args.id)
    t = schema.types.get(args.type) if args.type else None
    msg = schema.message_for(parts["port"], t)
    name = msg.name if msg else ("(node-type port; pass --type)" if parts["port"] >
                                 schema.protocol_last_port else "(unused protocol port)")
    print(f"{args.id:#010x}: priority {parts['priority']}, dst {parts['dst']}, "
          f"port {parts['port']} {name}, src {parts['src']}")
    return 0


def cmd_decode(args: argparse.Namespace) -> int:
    schema = _schema(args)
    data = bytes.fromhex(args.data.replace(" ", "").replace(":", ""))
    t = schema.types.get(args.type) if args.type else None
    d = codec.decode_frame(schema, args.id, data, t, raw=args.raw)
    who = f"{d.src} -> {'all' if d.dst == 0 else d.dst}"
    if d.message is None:
        print(f"port {d.port} ({who}): no schema message" + (" (pass --type)" if not t else ""))
        return 1
    print(f"{d.message.name} ({who}, priority {d.priority})")
    if d.error:
        print(f"  error: {d.error}")
        return 1
    for k, v in (d.values or {}).items():
        print(f"  {k} = {v}")
    return 0


def cmd_encode(args: argparse.Namespace) -> int:
    schema = _schema(args)
    t = schema.protocol if args.type == "protocol" else schema.types[args.type]
    m = t.message(args.message)
    values = {}
    for kv in args.values:
        k, _, v = kv.partition("=")
        try:
            values[k] = ast.literal_eval(v)
        except (ValueError, SyntaxError):
            values[k] = v                       # an enum label
    data = codec.encode(m, values, raw=args.raw)
    can_id = codec.make_id(schema, m, src=args.src, dst=args.dst)
    print(f"{can_id:08X}#{data.hex().upper()}   (cansend format)")
    return 0


def cmd_dbc(args: argparse.Namespace) -> int:
    schema = _schema(args)
    bus = gen_dbc.load_bus(args.bus, schema)
    text = gen_dbc.generate(schema, bus)
    if args.output:
        Path(args.output).write_text(text, encoding="utf-8")
        print(f"wrote {args.output}")
    else:
        sys.stdout.write(text)
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="anv-schema", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir",
                    help="schema directory (default: $ANV_SCHEMA_DIR or the repo's schema/)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("check", help="validate the schema").set_defaults(fn=cmd_check)

    g = sub.add_parser("gen", help="generate C code and schema.json")
    g.add_argument("--check", action="store_true", help="only report whether output is current")
    g.add_argument("--out", help="output directory (default: <schema dir>/generated)")
    g.set_defaults(fn=cmd_gen)

    s = sub.add_parser("show", help="describe the types")
    s.add_argument("type", nargs="?")
    s.set_defaults(fn=cmd_show)

    i = sub.add_parser("id", help="split a 29-bit ID")
    i.add_argument("id", type=_int)
    i.add_argument("--type", help="node type of the sender, to name node-type ports")
    i.set_defaults(fn=cmd_id)

    d = sub.add_parser("decode", help="decode one frame")
    d.add_argument("id", type=_int)
    d.add_argument("data", help="hex bytes, e.g. 0203A00F")
    d.add_argument("--type", help="node type of the sender (or the receiver, for commands)")
    d.add_argument("--raw", action="store_true", help="show raw integers")
    d.set_defaults(fn=cmd_decode)

    e = sub.add_parser("encode", help="encode one frame")
    e.add_argument("type", help="node type, or 'protocol'")
    e.add_argument("message")
    e.add_argument("values", nargs="*", help="field=value (lists like speed=[1.0,2.0])")
    e.add_argument("--src", type=_int, default=0)
    e.add_argument("--dst", type=_int, default=0)
    e.add_argument("--raw", action="store_true", help="values are raw integers")
    e.set_defaults(fn=cmd_encode)

    b = sub.add_parser("dbc", help="write a DBC for a bus layout")
    b.add_argument("bus", help="bus layout YAML (schema/buses/*.yaml)")
    b.add_argument("-o", "--output")
    b.set_defaults(fn=cmd_dbc)

    args = ap.parse_args(argv)
    try:
        return args.fn(args)
    except SchemaError as ex:
        print(ex, file=sys.stderr)
        return 1
    except (KeyError, ValueError) as ex:
        print(f"error: {ex}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
