"""Write a DBC file for a concrete bus, so SavvyCAN, cantools and similar tools can decode it.

A DBC lists fixed IDs, but here an ID depends on who sends to whom, so a DBC is made for a
*bus layout*: which node IDs exist and what type each one is (schema/buses/*.yaml).

Included: everything each node sends to everyone (dst 0), everything each host sends to
everyone, and each host's commands to each node. Left out: frames addressed to one node
that aren't commands (SERVICE requests and replies, NMT to one node, ISO-TP transfers),
since their IDs multiply with every host-node pair; tools still show those as raw frames.
"""
from __future__ import annotations

from collections import OrderedDict
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml

from .model import Message, NodeType, Schema

BUS_KEYS = {"name", "description", "hosts", "nodes"}


@dataclass
class BusNode:
    id: int
    name: str
    type: NodeType | None          # None for hosts


@dataclass
class BusLayout:
    name: str
    hosts: list[BusNode]
    nodes: list[BusNode]


def load_bus(path: str | Path, schema: Schema) -> BusLayout:
    with open(path, encoding="utf-8") as f:
        raw: dict[str, Any] = yaml.safe_load(f) or {}
    unknown = set(raw) - BUS_KEYS
    if unknown:
        raise ValueError(f"{path}: unknown key(s) {', '.join(sorted(unknown))}")
    hosts = [BusNode(int(h["id"]), str(h["name"]), None) for h in raw.get("hosts", [])]
    nodes = []
    for n in raw.get("nodes", []):
        tname = n["type"]
        if tname not in schema.types:
            raise ValueError(f"{path}: node {n.get('name')} has unknown type {tname!r}")
        nodes.append(BusNode(int(n["id"]), str(n.get("name", f"node{n['id']}")),
                             schema.types[tname]))
    ids = [b.id for b in hosts + nodes]
    if len(ids) != len(set(ids)):
        raise ValueError(f"{path}: a node ID is used twice")
    return BusLayout(str(raw.get("name", Path(path).stem)), hosts, nodes)


def _signals(m: Message, receivers: list[str]):
    from cantools.database.can import Signal
    from cantools.database.conversion import BaseConversion

    out = []
    for f in m.fields:
        if f.name == "reserved" or f.name.startswith("reserved_"):
            continue
        choices = None
        if f.enum is not None:
            choices = OrderedDict(sorted((v, k) for k, v in f.enum.values.items()))
        for i in range(f.count):
            name = f"{f.name}_{i}" if f.count > 1 else f.name
            out.append(Signal(
                name=name, start=f.element_start(i), length=f.type.bits,
                byte_order="little_endian", is_signed=f.type.signed and not f.type.is_float,
                conversion=BaseConversion.factory(scale=f.scale, offset=f.offset,
                                                  choices=choices, is_float=f.type.is_float),
                minimum=f.min, maximum=f.max, unit=f.unit or None,
                comment=f.description or None, receivers=receivers))
    return out


def build(schema: Schema, bus: BusLayout):
    """A cantools Database for the bus."""
    import cantools
    from cantools.database.can import Message as DbcMessage
    from cantools.database.can import Node

    db = cantools.database.can.Database()
    host_names = [h.name for h in bus.hosts]
    node_names = [n.name for n in bus.nodes]
    db.nodes.extend(Node(n) for n in host_names + node_names)
    seen: set[int] = set()

    def add(m: Message, src: int, dst: int, sender: str, name: str, receivers: list[str]):
        if m.kind == "frame":
            return
        can_id = schema.id_layout.make(priority=m.priority, dst=dst, port=m.port, src=src)
        if can_id in seen:
            return
        seen.add(can_id)
        cycle = round(1000 / m.rate.default) if m.rate and m.rate.default > 0 else None
        db.messages.append(DbcMessage(
            frame_id=can_id, name=name, length=m.length, signals=_signals(m, receivers),
            senders=[sender], is_extended_frame=True, cycle_time=cycle,
            comment=m.description or None))

    proto = schema.protocol
    for n in bus.nodes:
        for m in proto.messages:
            if m.sender in ("node", "any"):
                add(m, n.id, 0, n.name, f"{n.name}_{m.name}", host_names)
        for m in n.type.messages:
            if m.sender == "node":
                add(m, n.id, 0, n.name, f"{n.name}_{m.name}", host_names)
    for h in bus.hosts:
        for m in proto.messages:
            if m.sender in ("host", "leader", "any"):
                add(m, h.id, 0, h.name, f"{h.name}_{m.name}", node_names + host_names)
        for n in bus.nodes:
            for m in n.type.messages:
                if m.sender in ("host", "leader", "any"):
                    add(m, h.id, n.id, h.name, f"{h.name}_to_{n.name}_{m.name}", [n.name])
    db.refresh()
    return db


def generate(schema: Schema, bus: BusLayout) -> str:
    return build(schema, bus).as_dbc_string()
