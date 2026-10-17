from __future__ import annotations

import random
import shutil
import struct
import textwrap
from pathlib import Path

import pytest

import anv_schema
from anv_schema.model import Message

REPO = Path(__file__).resolve().parents[4]
SCHEMA_DIR = REPO / "schema"


@pytest.fixture(scope="session")
def schema():
    return anv_schema.load(SCHEMA_DIR)


@pytest.fixture
def tmp_schema(tmp_path):
    """A copy of protocol.yaml in an empty schema dir; add types with write_type()."""
    (tmp_path / "types").mkdir()
    (tmp_path / "fragments").mkdir()
    shutil.copy(SCHEMA_DIR / "protocol.yaml", tmp_path / "protocol.yaml")
    return tmp_path


def write_type(root: Path, name: str, text: str, sub: str = "types") -> Path:
    path = root / sub / f"{name}.yaml"
    path.write_text(textwrap.dedent(text), encoding="utf-8")
    return path


def all_messages(schema) -> list[tuple[str, Message]]:
    out = [("protocol", m) for m in schema.protocol.messages]
    for t in schema.types.values():
        out += [(t.name, m) for m in t.messages]
    return [(t, m) for t, m in out if m.kind != "frame"]


def random_raw(msg: Message, rng: random.Random) -> dict:
    """Random raw values for every field (respecting const)."""
    values = {}
    for f in msg.fields:
        def one(f=f):
            if f.const is not None:
                return f.const
            if f.type.is_float:
                # a value that survives a round trip through f32
                return struct.unpack("<f", struct.pack("<f", rng.uniform(-1e6, 1e6)))[0]
            if f.type.is_bool:
                return rng.random() < 0.5
            return rng.randint(f.type.raw_min, f.type.raw_max)
        values[f.name] = [one() for _ in range(f.count)] if f.count > 1 else one()
    return values
