"""The C++ codec (through the bindings) must match anv_schema's Python codec byte for byte.

anv_schema's own tests prove Python == the generated C the ESP32 runs, so this closes the loop:
host C++ == firmware C. Uses the same random-vector idea as anv_schema/tests/test_gen_c.py.
"""
from __future__ import annotations

import math
import random
import struct
from pathlib import Path

import pytest

import anv_can

anv_schema = pytest.importorskip("anv_schema")

REPO = Path(__file__).resolve().parents[4]


@pytest.fixture(scope="module")
def py_schema():
    return anv_schema.load(REPO / "schema")


@pytest.fixture(scope="module")
def cpp_schema():
    return anv_can.load_schema(REPO / "schema" / "generated" / "schema.json")


def _types(py_schema):
    return [py_schema.protocol, *py_schema.types.values()]


def _random_raw(msg, rng):
    values = {}
    for f in msg.fields:
        def one(f=f):
            if f.const is not None:
                return f.const
            if f.type.is_float:
                return struct.unpack("<f", struct.pack("<f", rng.uniform(-1e6, 1e6)))[0]
            if f.type.is_bool:
                return rng.random() < 0.5
            return rng.randint(f.type.raw_min, f.type.raw_max)
        values[f.name] = [one() for _ in range(f.count)] if f.count > 1 else one()
    return values


def _random_phys(msg, rng):
    """Physical values inside each field's limits (labels for enums)."""
    values = {}
    for f in msg.fields:
        if f.const is not None:
            continue
        def one(f=f):
            if f.enum is not None and rng.random() < 0.7:
                return rng.choice(list(f.enum.values))
            if f.type.is_bool:
                return rng.random() < 0.5
            if f.type.is_float:
                return rng.uniform(-1000, 1000)
            lo = f.type.raw_min * f.scale + f.offset
            hi = f.type.raw_max * f.scale + f.offset
            lo, hi = min(lo, hi), max(lo, hi)
            lo = max(lo, f.min) if f.min is not None else lo
            hi = min(hi, f.max) if f.max is not None else hi
            if not f.is_scaled:
                return rng.randint(math.ceil(lo), math.floor(hi))
            return rng.uniform(lo, hi)
        values[f.name] = [one() for _ in range(f.count)] if f.count > 1 else one()
    return values


def _same(a, b) -> bool:
    if isinstance(b, list):
        return isinstance(a, list) and len(a) == len(b) and all(map(_same, a, b))
    if isinstance(b, float):
        return a == pytest.approx(b, rel=1e-12, abs=1e-12)
    return type(a) is type(b) and a == b


def test_schema_metadata_matches(py_schema, cpp_schema):
    by_id = sorted(py_schema.types.values(), key=lambda t: t.type_id)
    assert [t.name for t in cpp_schema.types] == [t.name for t in by_id]
    for pt in _types(py_schema):
        ct = cpp_schema.type(pt.name)
        assert (ct.type_id, ct.version, ct.wire_hash) == (pt.type_id, pt.version, pt.wire_hash)
        assert [m.name for m in ct.messages] == [m.name for m in pt.messages]
        assert [p.index for p in ct.params] == [p.index for p in pt.params]
        assert [e.code for e in ct.events] == [e.code for e in pt.events]
        for pm in pt.messages:
            cm = ct.message(pm.name)
            assert (cm.port, cm.priority, cm.length, cm.kind) == \
                (pm.port, pm.priority, pm.length, pm.kind)
            assert [(f.name, f.start_bit, f.count, f.type) for f in cm.fields] == \
                [(f.name, f.start_bit, f.count, f.type.name) for f in pm.fields]


def test_raw_vectors_byte_identical(py_schema, cpp_schema):
    rng = random.Random(11)
    n = 0
    for pt in _types(py_schema):
        ct = cpp_schema.type(pt.name)
        for pm in pt.messages:
            if pm.kind == "frame":
                continue
            cm = ct.message(pm.name)
            for _ in range(50):
                values = _random_raw(pm, rng)
                expected = anv_schema.encode(pm, values, raw=True)
                got = anv_can.encode(cm, values, raw=True)
                assert got == expected, (pt.name, pm.name, values)
                assert anv_can.decode(cm, expected, raw=True) == \
                    anv_schema.decode(pm, expected, raw=True)
                n += 1
    assert n > 500


def test_physical_vectors_byte_identical(py_schema, cpp_schema):
    rng = random.Random(5)
    for pt in _types(py_schema):
        ct = cpp_schema.type(pt.name)
        for pm in pt.messages:
            if pm.kind == "frame":
                continue
            cm = ct.message(pm.name)
            for _ in range(50):
                values = _random_phys(pm, rng)
                expected = anv_schema.encode(pm, values)
                assert anv_can.encode(cm, values) == expected, (pt.name, pm.name, values)
                want = anv_schema.decode(pm, expected)
                got = anv_can.decode(cm, expected)
                assert got.keys() == want.keys()
                for k, v in want.items():
                    assert _same(got[k], v), (pt.name, pm.name, k, got[k], v)


def test_errors_match(py_schema, cpp_schema):
    pm = py_schema.types["motor_controller"].message("setpoint")
    cm = cpp_schema.type("motor_controller").message("setpoint")
    bad = [{"mode": "warp"}, {"target": [1e9, 0]}, {"nope": 1}, {"target": [1, 2, 3]}]
    for values in bad:
        with pytest.raises(anv_schema.CodecError):
            anv_schema.encode(pm, values)
        with pytest.raises(anv_can.CodecError):
            anv_can.encode(cm, values)
    with pytest.raises(anv_can.CodecError):
        anv_can.decode(cm, b"\x00")


def test_ids_match(py_schema, cpp_schema):
    rng = random.Random(2)
    for _ in range(5000):
        parts = {"priority": rng.randrange(8), "dst": rng.randrange(256),
                 "port": rng.randrange(1024), "src": rng.randrange(256)}
        can_id = py_schema.id_layout.make(**parts)
        assert cpp_schema.make_id(**parts) == can_id
        assert cpp_schema.parse_id(can_id) == py_schema.id_layout.parse(can_id)
    with pytest.raises(ValueError):
        cpp_schema.make_id(8, 0, 0, 0)


def test_decode_frame_matches(py_schema, cpp_schema):
    pt = py_schema.types["motor_controller"]
    pm = pt.message("setpoint")
    data = anv_schema.encode(pm, {"mode": "velocity", "target": [1.5, -2.0]})
    can_id = anv_schema.make_id(py_schema, pm, src=1, dst=20)
    want = anv_schema.decode_frame(py_schema, can_id, data, pt)
    got = anv_can.decode_frame(cpp_schema, can_id, data, cpp_schema.type("motor_controller"))
    assert (got.src, got.dst, got.port, got.priority) == \
        (want.src, want.dst, want.port, want.priority)
    assert got.message.name == want.message.name
    assert got.values["mode"] == "velocity"
    assert got.values["target"] == pytest.approx(want.values["target"])
    assert anv_can.decode_frame(cpp_schema, can_id, data).message is None
