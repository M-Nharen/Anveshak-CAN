# anv_schema (Python)

Loads, checks and encodes/decodes the CAN schema in `schema/`, and generates the C code and
`schema.json` from it. The format and how to use the output are in **`schema/README.md`**.

```sh
pip install -e host/python/anv_schema           # adds the `anv-schema` command
pip install -e "host/python/anv_schema[test]"   # plus pytest and cantools
pytest host/python/anv_schema
```

| Module | Job |
|---|---|
| `model.py` | The schema as dataclasses: `Schema`, `NodeType`, `Message`, `Field`, `Param`, ... |
| `loader.py` | YAML -> model: includes, `count` expansion, enums, bit layout. `load()` |
| `validate.py` | Cross-item rules (duplicates, ranges, overlaps). Reports every problem |
| `codec.py` | `encode()` / `decode()` / `decode_frame()` / `make_id()` |
| `hashing.py` | The wire hash (what changes when compatibility does) |
| `bundle.py` | `schema.json` writer and reader (`load_bundle()`) |
| `gen_c.py` | C11 for firmware |
| `gen_dbc.py` | DBC for a concrete bus layout |
| `cli.py` | `anv-schema check / gen / show / id / encode / decode / dbc` |

Only `PyYAML` is required; `cantools` is needed for `anv-schema dbc` and the tests.

The tests include compiling the generated C with strict warnings and checking it produces the
same bytes as the Python codec for random values of every message, and that `cantools`
decodes our frames to the same values from the generated DBC.
