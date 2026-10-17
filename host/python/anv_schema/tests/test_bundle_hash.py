from __future__ import annotations

import json

from conftest import SCHEMA_DIR, write_type

import anv_schema
from anv_schema import bundle


def test_bundle_round_trip(schema):
    d = bundle.to_dict(schema)
    again = bundle.from_dict(json.loads(json.dumps(d)))
    assert bundle.to_dict(again) == d


def test_committed_bundle_loads(schema):
    s = anv_schema.load_bundle(SCHEMA_DIR / "generated" / "schema.json")
    assert set(s.types) == set(schema.types)
    for name, t in schema.types.items():
        assert s.types[name].wire_hash == t.wire_hash


def _hash(root, text: str) -> int:
    write_type(root, "t", text)
    return anv_schema.load(root).types["t"].wire_hash


BASE = """\
name: t
type_id: 5
messages:
  - {name: m, port: 64, kind: event, description: hello, fields: [{name: a, type: u8, unit: V}]}
params:
  - {index: 1, name: p, type: u8, default: 3, max: 10}
"""


def test_hash_ignores_text_and_defaults(tmp_schema):
    h = _hash(tmp_schema, BASE)
    assert _hash(tmp_schema, BASE.replace("hello", "bye").replace("unit: V", "unit: mV")) == h
    assert _hash(tmp_schema, BASE.replace("default: 3", "default: 4")) == h


def test_hash_changes_with_the_wire(tmp_schema):
    h = _hash(tmp_schema, BASE)
    assert _hash(tmp_schema, BASE.replace("type: u8, unit", "type: u16, unit")) != h
    assert _hash(tmp_schema, BASE.replace("port: 64", "port: 65")) != h
    assert _hash(tmp_schema, BASE.replace("index: 1", "index: 2")) != h
    assert _hash(tmp_schema, BASE.replace("type_id: 5", "type_id: 5\nversion: 2")) != h
