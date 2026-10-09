"""Thin wrapper around the generated error registry (defs/errors.yaml), for the Errors page.

Every error code shown on the dashboard comes with its explanation from one source of truth,
built by defs/codegen/generate.py; this module just points sys.path at it (it is a plain
directory, not an installed package, like host/sim's own use of defs/generated).
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any

_GENERATED = Path(__file__).resolve().parents[3] / "defs" / "generated" / "python"
if str(_GENERATED) not in sys.path:
    sys.path.insert(0, str(_GENERATED))

from anv_can_defs.errors import lookup


def explain(code: int) -> dict[str, Any] | None:
    """{"name", "severity", "description", "likely_cause"} for a known code, else None."""
    info = lookup(code)
    if info is None:
        return None
    return {
        "name": info.name,
        "severity": int(info.severity),
        "description": info.description,
        "likely_cause": info.likely_cause,
    }
