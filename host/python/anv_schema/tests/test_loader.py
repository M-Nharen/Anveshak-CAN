from __future__ import annotations

import pytest
from conftest import write_type

import anv_schema
from anv_schema.loader import SchemaError


def test_repo_schema_is_valid(schema):
    assert anv_schema.validate(schema) == []
    assert schema.protocol.type_id == 0
    assert "esp32_bench" in schema.types


def test_id_layout_is_29_bits_priority_first(schema):
    lay = schema.id_layout
    assert lay.fields[0].name == "priority"
    assert sum(f.bits for f in lay.fields) == 29
    can_id = lay.make(priority=2, dst=16, port=64, src=1)
    assert lay.parse(can_id) == {"priority": 2, "dst": 16, "port": 64, "src": 1}
    assert can_id < (1 << 29)


def test_dst_is_inside_the_top_16_bits_for_twai_dual_filters(schema):
    dst = schema.id_layout.get("dst")
    assert dst.shift >= 13, "ESP32 TWAI dual filter only compares ID bits 28..13"


def test_fields_are_laid_out_back_to_back(schema):
    hb = schema.protocol.message("heartbeat")
    assert [f.start_bit for f in hb.fields][:3] == [0, 8, 10]
    assert hb.length == 8


def test_param_count_expands_to_consecutive_indices(schema):
    t = schema.types["motor_controller"]
    assert [t.param(f"kp_{i}").index for i in range(2)] == [0, 1]
    assert t.param("kp_1").group == "kp" or t.param("kp_1").group == "pid"


def test_include_with_prefix_and_offsets(schema):
    t = schema.types["analog_sensor"]
    assert t.message("main_power").port == 200
    assert t.message("aux_power").port == 201
    assert t.param("aux_undervoltage_v").index == 0x0E10
    assert t.event("aux_supply_low").code == 0x7E11


def test_enum_list_form(schema):
    mode = schema.types["motor_controller"].enums["mode"]
    assert mode.values == {"coast": 0, "brake": 1, "velocity": 2, "duty": 3}


def test_type_can_use_protocol_enums(tmp_schema):
    write_type(tmp_schema, "t", """\
        name: t
        type_id: 5
        messages:
          - name: m
            port: 64
            kind: event
            fields: [{name: s, type: u8, enum: node_state}]
        """)
    s = anv_schema.load(tmp_schema)
    assert s.types["t"].message("m").fields[0].enum.name == "node_state"


def test_unknown_key_is_reported_with_location(tmp_schema):
    # The classic YAML trap: a comma inside an unquoted value in {...} makes a new key.
    write_type(tmp_schema, "t", """\
        name: t
        type_id: 5
        params:
          - {index: 1, name: p, type: u8, description: big, very big}
        """)
    with pytest.raises(SchemaError) as e:
        anv_schema.load(tmp_schema)
    assert "param p" in str(e.value) and "very big" in str(e.value)


def test_missing_include_is_reported(tmp_schema):
    write_type(tmp_schema, "t", "name: t\ntype_id: 5\ninclude: [nope.yaml]\n")
    with pytest.raises(SchemaError, match="nope.yaml"):
        anv_schema.load(tmp_schema)


def test_too_many_bits(tmp_schema):
    write_type(tmp_schema, "t", """\
        name: t
        type_id: 5
        messages:
          - {name: m, port: 64, kind: event, fields: [{name: a, type: u32, count: 3}]}
        """)
    with pytest.raises(SchemaError, match="96 bits"):
        anv_schema.load(tmp_schema)


def test_stream_needs_a_rate(tmp_schema):
    write_type(tmp_schema, "t", """\
        name: t
        type_id: 5
        messages:
          - {name: m, port: 64, kind: stream, fields: [{name: a, type: u8}]}
        """)
    with pytest.raises(SchemaError, match="rate"):
        anv_schema.load(tmp_schema)


def test_default_priorities(tmp_schema):
    write_type(tmp_schema, "t", """\
        name: t
        type_id: 5
        messages:
          - {name: fast, port: 64, rate: 50, fields: [{name: a, type: u8}]}
          - {name: slow, port: 65, rate: 1, fields: [{name: a, type: u8}]}
          - {name: cmd, port: 66, kind: command, fields: [{name: a, type: u8}]}
        """)
    t = anv_schema.load(tmp_schema).types["t"]
    assert (t.message("fast").priority, t.message("slow").priority,
            t.message("cmd").priority) == (2, 3, 1)
    assert t.message("cmd").sender == "leader"


def test_id_layout_must_total_29(tmp_schema):
    p = tmp_schema / "protocol.yaml"
    p.write_text(p.read_text().replace("{field: port, bits: 10}", "{field: port, bits: 9}"))
    with pytest.raises(SchemaError, match="28"):
        anv_schema.load(tmp_schema)


def test_id_layout_widths_are_configurable(tmp_schema):
    p = tmp_schema / "protocol.yaml"
    text = p.read_text().replace("{field: port, bits: 10}", "{field: port, bits: 9}")
    text = text.replace("{field: src, bits: 8}", "{field: src, bits: 9}")
    p.write_text(text)
    s = anv_schema.load(tmp_schema)
    assert s.max_port == 511
    assert s.id_layout.get("src").bits == 9


def test_select_types(schema):
    from conftest import SCHEMA_DIR
    s = anv_schema.load(SCHEMA_DIR, types=["esp32_bench"])
    assert list(s.types) == ["esp32_bench"]
    with pytest.raises(SchemaError, match="nope"):
        anv_schema.load(SCHEMA_DIR, types=["nope"])
