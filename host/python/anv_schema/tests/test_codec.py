from __future__ import annotations

import math
import random

import pytest
from conftest import SCHEMA_DIR, all_messages, random_raw

import anv_schema
from anv_schema import CodecError, codec, gen_dbc


def test_raw_round_trip_every_message(schema):
    rng = random.Random(1)
    for _type, msg in all_messages(schema):
        for _ in range(50):
            values = random_raw(msg, rng)
            data = anv_schema.encode(msg, values, raw=True)
            assert len(data) == msg.length
            assert anv_schema.decode(msg, data, raw=True) == values, msg.name


def test_physical_values_and_enum_labels(schema):
    m = schema.types["motor_controller"].message("setpoint")
    data = anv_schema.encode(m, {"mode": "velocity", "target": [1.5, -2.25], "accel_limit": 10})
    assert data.hex() == "02" "9600" "1fff" "e803"
    back = anv_schema.decode(m, data)
    assert back["mode"] == "velocity"
    assert back["target"] == pytest.approx([1.5, -2.25])
    assert back["accel_limit"] == pytest.approx(10)


def test_const_fields(schema):
    estop = schema.protocol.message("estop")
    data = anv_schema.encode(estop, {"reason": 3})
    assert data == bytes([0xE5, 3])
    with pytest.raises(CodecError, match="0xe5"):
        anv_schema.decode(estop, bytes([0x00, 3]))
    assert anv_schema.decode(estop, bytes([0x00, 3]), strict=False)["magic"] == 0


def test_sub_byte_fields(schema):
    hb = schema.protocol.message("heartbeat")
    data = anv_schema.encode(hb, {"state": "armed", "can_state": "passive", "debug_level": 2,
                                  "leader_seen": True, "type_id": 0x1234,
                                  "schema_hash": 0xBEEF, "uptime_s": 7})
    assert data[1] == 0b0010_1001          # can_state=1 (bits 8-9), debug=2 (10-11), bit 13
    d = anv_schema.decode(hb, data)
    assert (d["state"], d["can_state"], d["debug_level"], d["leader_seen"]) == \
        ("armed", "passive", 2, True)


def test_out_of_range_is_refused(schema):
    m = schema.types["motor_controller"].message("setpoint")
    with pytest.raises(CodecError, match="doesn't fit"):
        anv_schema.encode(m, {"target": [400.0, 0]})
    with pytest.raises(CodecError, match="not a label"):
        anv_schema.encode(m, {"mode": "warp"})
    with pytest.raises(CodecError, match="no field"):
        anv_schema.encode(m, {"speed": 1})
    with pytest.raises(CodecError, match="needs 2 values"):
        anv_schema.encode(m, {"target": [1, 2, 3]})


def test_physical_min_max(schema):
    m = schema.types["motor_controller"].message("power")
    with pytest.raises(CodecError, match="outside"):
        anv_schema.encode(m, {"voltage": -1})
    anv_schema.encode(m, {"voltage": -1 + 1}, strict=True)


def test_f32_field(tmp_schema):
    from conftest import write_type
    write_type(tmp_schema, "t", """\
        name: t
        type_id: 5
        messages:
          - {name: m, port: 64, kind: event,
             fields: [{name: x, type: f32}, {name: y, type: i5}]}
        """)
    m = anv_schema.load(tmp_schema).types["t"].message("m")
    data = anv_schema.encode(m, {"x": 1.5, "y": -16})
    assert len(data) == 5
    d = anv_schema.decode(m, data)
    assert d == {"x": 1.5, "y": -16}


def test_decode_frame_finds_the_message(schema):
    t = schema.types["esp32_bench"]
    m = t.message("status")
    can_id = anv_schema.make_id(schema, m, src=16)
    data = anv_schema.encode(m, {"chip_temp": 41.5, "free_heap": 200})
    d = anv_schema.decode_frame(schema, can_id, data, t)
    assert (d.src, d.dst, d.port, d.message.name) == (16, 0, 64, "status")
    assert d.values["chip_temp"] == pytest.approx(41.5)
    # protocol ports decode without knowing the type
    hb = schema.protocol.message("heartbeat")
    d = anv_schema.decode_frame(schema, anv_schema.make_id(schema, hb, src=16),
                                anv_schema.encode(hb, {"state": "idle"}))
    assert d.message.name == "heartbeat" and d.values["state"] == "idle"
    # a node-type port with no type known
    assert anv_schema.decode_frame(schema, can_id, data).message is None


def test_physical_range(schema):
    f = schema.types["motor_controller"].message("feedback").field("current")
    assert codec.physical_range(f) == pytest.approx((0, 51))
    f32 = schema.types["motor_controller"].message("setpoint").field("mode")
    assert codec.physical_range(f32) == (0, 255)
    assert not math.isinf(codec.physical_range(f32)[1])


def test_layout_matches_cantools(schema):
    """An independent DBC implementation decodes our bytes to the same values."""
    pytest.importorskip("cantools")
    bus = gen_dbc.load_bus(SCHEMA_DIR / "buses" / "bench.yaml", schema)
    db = gen_dbc.build(schema, bus)
    rng = random.Random(7)
    checked = 0
    for dbc_msg in db.messages:
        parts = schema.id_layout.parse(dbc_msg.frame_id)
        src_type = next((n.type for n in bus.nodes if n.id == parts["src"]), None)
        dst_type = next((n.type for n in bus.nodes if n.id == parts["dst"]), None)
        msg = schema.message_for(parts["port"], src_type or dst_type)
        assert msg is not None
        for _ in range(20):
            values = random_raw(msg, rng)
            data = anv_schema.encode(msg, values, raw=True)
            theirs = dbc_msg.decode(data, decode_choices=False, scaling=False)
            for f in msg.fields:
                if f.name.startswith("reserved"):
                    continue
                vals = values[f.name] if f.count > 1 else [values[f.name]]
                for i, v in enumerate(vals):
                    key = f"{f.name}_{i}" if f.count > 1 else f.name
                    assert theirs[key] == pytest.approx(int(v) if f.type.is_bool else v), \
                        (dbc_msg.name, key)
        checked += 1
    assert checked > 20
