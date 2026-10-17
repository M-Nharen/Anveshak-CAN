"""A fake node for the anv_host tests, written against the schema only: heartbeat, ID request,
the parameter service, stream rates, NMT from the leader, E-stop and the command watchdog."""
from __future__ import annotations

import struct
import threading
import time
from typing import Any

import anv_can


def _label(schema: anv_can.Schema, enum: str, label: str) -> int:
    return schema.protocol.enums[enum][label]


class FakeNode:
    def __init__(self, bus: anv_can.TestBus | anv_can.Link, schema: anv_can.Schema, type_name: str,
                 node_id: int, mac: int = 0) -> None:
        self.s = schema
        self.t = schema.type(type_name)
        self.link = bus.connect() if isinstance(bus, anv_can.TestBus) else bus
        self.id = node_id
        self.mac = mac
        self.armed = False
        self.estop = False
        self.leader = 0
        self.term = 0
        self.commands = 0
        self.actions: list[tuple[int, int]] = []
        self.saves = 0
        self.debug_level = 0
        self.debug_sets = 0
        self.info_pages = {"chip": 1, "fw_build": 0, "type_id": self.t.type_id,
                           "schema_hash": self.t.schema_hash16}
        self.params: dict[int, int] = {}
        for p in list(self.t.params) + list(self.s.protocol.params):
            self.params[p.index] = self._wire(p.type, p.default or 0)
        self.rates = {m.port: int(m.rate["default"] * 1000) for m in self.t.messages
                      if m.kind == "stream" and m.rate}
        self._last_cmd = 0.0
        self._run = True
        self._th = threading.Thread(target=self._loop, daemon=True)
        self._th.start()

    @staticmethod
    def _wire(type_name: str, v: float) -> int:
        if type_name == "f32":
            return struct.unpack("<I", struct.pack("<f", v))[0]
        return int(v) & 0xFFFFFFFF

    def stop(self) -> None:
        self._run = False
        self._th.join()

    def send_event(self, code: int, detail: int = 0, severity: int = 0, cleared: bool = False,
                   seq: int = 1) -> None:
        self._send(self.s.protocol.message("event"), {
            "severity": severity, "cleared": cleared, "code": code, "seq": seq, "detail": detail,
            "uptime_s": 5}, 0, raw=False)

    def _send(self, msg: anv_can.Message, values: dict[str, Any], dst: int,
              raw: bool = True) -> None:
        src = self.id or self.s.node_ids["unassigned"]
        self.link.send(anv_can.Frame(anv_can.make_id(self.s, msg, src=src, dst=dst),
                                     anv_can.encode(msg, values, raw=raw)))

    def _loop(self) -> None:
        p = self.s.protocol
        next_hb = 0.0
        next_stream: dict[int, float] = {}
        while self._run:
            try:
                f = self.link.recv(2)
            except EOFError:
                return
            if f is not None:
                self._handle(f)
            now = time.monotonic()
            if self.armed and now - self._last_cmd > 0.1:
                self.armed = False  # command watchdog
            if now >= next_hb:
                next_hb = now + 0.05
                if not self.id:
                    mac = list(self.mac.to_bytes(6, "big"))
                    self._send(p.message("id_request"), {"mac": mac, "type_id": self.t.type_id}, 0)
                    continue
                state = "estop" if self.estop else "armed" if self.armed else "idle"
                self._send(p.message("heartbeat"), {
                    "state": state, "type_id": self.t.type_id, "schema_hash": self.t.schema_hash16,
                    "uptime_s": 5, "leader_seen": bool(self.leader)}, 0, raw=False)
            for port, mhz in self.rates.items():
                if mhz and now >= next_stream.get(port, 0) and self.id:
                    next_stream[port] = now + 1000.0 / mhz
                    self._send(self.t.message(port), {}, 0)

    def _handle(self, f: anv_can.Frame) -> None:
        idp = self.s.parse_id(f.id)
        port, src, dst = idp["port"], idp["src"], idp["dst"]
        m = self.s.message_for(port, self.t)
        if m is None or m.kind == "frame":
            return
        v = anv_can.decode(m, f.data, raw=True, labels=False)
        name = m.name if port <= self.s.protocol_last_port else None
        to_me = dst == 0 or (self.id and dst == self.id)
        if name == "host_heartbeat" and v["role"] == _label(self.s, "host_role", "leader"):
            if (src, v["term"]) != (self.leader, self.term):
                self.armed = False
            self.leader, self.term = src, v["term"]
        elif name == "estop":
            self.estop, self.armed = True, False
        elif name == "estop_release" and src == self.leader:
            self.estop = False
        elif name == "nmt" and to_me and src == self.leader and v["term"] == self.term:
            if v["command"] == _label(self.s, "nmt_command", "arm") and not self.estop:
                self.armed, self._last_cmd = True, time.monotonic()
            elif v["command"] == _label(self.s, "nmt_command", "disarm"):
                self.armed = False
        elif name == "debug_level" and to_me and src == self.leader:
            self.debug_level, self.debug_sets = v["level"], self.debug_sets + 1
        elif name == "id_assign" and not self.id and dst == self.s.node_ids["unassigned"]:
            if bytes(v["mac"]) == self.mac.to_bytes(6, "big"):
                self.id = v["node_id"]
        elif name == "service" and self.id and dst == self.id:
            self._service(src, v)
        elif m.kind == "command" and port > self.s.protocol_last_port and dst == self.id \
                and src == self.leader:
            self.commands += 1
            self._last_cmd = time.monotonic()

    def _service(self, src: int, v: dict[str, Any]) -> None:
        ops = self.s.protocol.enums["service_op"]
        op, index, status = v["op"], v["index"], "ok"
        if op == ops["get"]:
            if index in self.params:
                v["value"] = self.params[index]
            else:
                status = "bad_index"
        elif op == ops["set"]:
            if index in self.params:
                self.params[index] = v["value"]
            else:
                status = "bad_index"
        elif op == ops["save"]:
            self.saves += 1
        elif op == ops["get_rate"]:
            v["value"] = self.rates.get(index, 0)
        elif op == ops["set_rate"]:
            self.rates[index] = v["value"]
        elif op == ops["action"]:
            self.actions.append((index, v["value"]))
        elif op == ops["info"]:
            pages = self.s.protocol.enums["info_page"]
            name = next((n for n, i in pages.items() if i == index), None)
            if name is None or name not in self.info_pages:
                status = "bad_index"
            else:
                v["value"] = self.info_pages[name]
        else:
            status = "bad_op"
        v["status"] = _label(self.s, "service_status", status)
        self._send(self.s.protocol.message("service"), v, src)


def eventually(cond, timeout: float = 2.0) -> bool:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.005)
    return bool(cond())
