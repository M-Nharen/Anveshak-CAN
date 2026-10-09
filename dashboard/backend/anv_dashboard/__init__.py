"""Anveshak dashboard backend: FastAPI + WebSocket in front of the gateway's local bridge socket."""

from __future__ import annotations

import sys
from pathlib import Path

# defs/generated/python holds anv_can_defs (parameter tables, error codes, IDs), generated from
# defs/rover.dbc, defs/errors.yaml and defs/params/*.yaml by defs/codegen/generate.py. It is not
# an installable package yet, so this is added to sys.path once, here, rather than duplicating
# any of its tables by hand (CLAUDE.md: "Never hand-write byte packing: use generated code").
_defs_python = Path(__file__).resolve().parents[3] / "defs" / "generated" / "python"
if _defs_python.is_dir() and str(_defs_python) not in sys.path:
    sys.path.insert(0, str(_defs_python))
