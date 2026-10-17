"""anv_schema: the CAN schema (node types, messages, parameters) as Python objects.

    import anv_schema
    s = anv_schema.load()                         # schema/ in this repo, or $ANV_SCHEMA_DIR
    t = s.types["esp32_bench"]
    data = anv_schema.encode(t.message("sensors"), {"chip_temp": 41.5})
    can_id = anv_schema.make_id(s, t.message("sensors"), src=16)
    values = anv_schema.decode(t.message("sensors"), data)

Or, without the YAML sources: anv_schema.load_bundle("schema/generated/schema.json").
"""
from .bundle import load_bundle
from .codec import CodecError, DecodedFrame, decode, decode_frame, encode, make_id
from .loader import SchemaError, default_schema_dir, load, load_type_file
from .model import (
    Action,
    Enum,
    Event,
    Field,
    IdLayout,
    Message,
    NodeType,
    Param,
    Schema,
)
from .validate import check, validate

__all__ = [
    "Action", "CodecError", "DecodedFrame", "Enum", "Event", "Field", "IdLayout", "Message",
    "NodeType", "Param", "Schema", "SchemaError", "check", "decode", "decode_frame",
    "default_schema_dir", "encode", "load", "load_bundle", "load_type_file", "make_id",
    "validate",
]
