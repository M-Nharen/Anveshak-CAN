from __future__ import annotations

from conftest import write_type

import anv_schema


def problems(root, text: str) -> list[str]:
    write_type(root, "t", text)
    return anv_schema.validate(anv_schema.load(root))


def test_duplicate_port_and_name(tmp_schema):
    p = problems(tmp_schema, """\
        name: t
        type_id: 5
        messages:
          - {name: a, port: 64, kind: event, fields: [{name: x, type: u8}]}
          - {name: a, port: 64, kind: event, fields: [{name: x, type: u8}]}
        """)
    assert any("two messages named a" in s for s in p)
    assert any("two messages on port 64" in s for s in p)


def test_type_ports_cannot_use_protocol_range(tmp_schema):
    p = problems(tmp_schema, """\
        name: t
        type_id: 5
        messages: [{name: a, port: 10, kind: event, fields: [{name: x, type: u8}]}]
        """)
    assert any("port 10 outside 64..1023" in s for s in p)


def test_overlapping_fields(tmp_schema):
    p = problems(tmp_schema, """\
        name: t
        type_id: 5
        messages:
          - name: a
            port: 64
            kind: event
            fields:
              - {name: x, type: u16}
              - {name: y, type: u8, start_bit: 8}
        """)
    assert any("overlap" in s for s in p)


def test_param_ranges(tmp_schema):
    p = problems(tmp_schema, """\
        name: t
        type_id: 5
        params:
          - {index: 1, name: a, type: u8, default: 300}
          - {index: 2, name: b, type: u8, min: 5, max: 10, default: 1}
          - {index: 3, name: c, type: i16, default: 1.5}
          - {index: 0xF100, name: d, type: u8}
        """)
    assert any("param a" in s and "doesn't fit u8" in s for s in p)
    assert any("param b" in s and "outside min/max" in s for s in p)
    assert any("param c" in s and "whole number" in s for s in p)
    assert any("param d" in s and "node-type parameters" in s for s in p)


def test_field_min_max_must_be_carriable(tmp_schema):
    p = problems(tmp_schema, """\
        name: t
        type_id: 5
        messages:
          - name: a
            port: 64
            kind: event
            fields: [{name: v, type: u8, scale: 0.1, max: 100}]
        """)
    assert any("max 100" in s and "0..25.5" in s for s in p)


def test_event_code_ranges(tmp_schema):
    p = problems(tmp_schema, """\
        name: t
        type_id: 5
        events: [{code: 0x0101, name: e, severity: error}]
        """)
    assert any("node-type events use codes" in s for s in p)


def test_type_id_rules(tmp_schema):
    write_type(tmp_schema, "u", "name: u\ntype_id: 5\n")
    p = problems(tmp_schema, "name: t\ntype_id: 5\n")
    assert any("type_id 5 already used" in s for s in p)
    p = problems(tmp_schema, "name: t\ntype_id: 1\n")
    assert any("type_id must be 2.." in s for s in p)


def test_c_keywords_and_collisions(tmp_schema):
    p = problems(tmp_schema, """\
        name: host
        type_id: 5
        messages:
          - {name: heartbeat, port: 64, kind: event, fields: [{name: default, type: u8}]}
        """)
    assert any("protocol message host_heartbeat" in s for s in p)
    assert any("field default" in s for s in p)


def test_commands_come_from_hosts(tmp_schema):
    p = problems(tmp_schema, """\
        name: t
        type_id: 5
        messages:
          - {name: c, port: 64, kind: command, sender: node, fields: [{name: x, type: u8}]}
          - {name: s, port: 65, rate: 10, watchdog_ms: 5, fields: [{name: x, type: u8}]}
        """)
    assert any("commands are sent by hosts" in s for s in p)
    assert any("watchdog_ms and apply only make sense on commands" in s for s in p)


def test_protocol_param_name_clash(tmp_schema):
    p = problems(tmp_schema, """\
        name: t
        type_id: 5
        params: [{index: 1, name: heartbeat_hz, type: u8}]
        """)
    assert any("clashes with a protocol parameter" in s for s in p)
