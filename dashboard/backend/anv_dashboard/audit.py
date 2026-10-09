"""Audit and undo (docs/architecture.md "Control panel", part 7): every control action is kept
as a CONTROL_ACTION entry with its old and new value; "Undo last change" restores the previous
value of any rate, parameter or override. Pure in-memory log, no gateway involved.
"""

from __future__ import annotations

import itertools
import time
from collections.abc import Callable
from typing import Any

MAX_ENTRIES = 200


class AuditLog:
    def __init__(self, clock: Callable[[], float] = time.time) -> None:
        self._clock = clock
        self._entries: list[dict[str, Any]] = []
        self._ids = itertools.count(1)

    def record(self, kind: str, who: str, **fields: Any) -> dict[str, Any]:
        """kind: "rate" | "param" | "refused" | "undo". Returns the stored entry."""
        entry = {"id": next(self._ids), "ts": self._clock(), "kind": kind, "who": who, **fields}
        self._entries.append(entry)
        del self._entries[:-MAX_ENTRIES]
        return entry

    def recent(self, limit: int = 20) -> list[dict[str, Any]]:
        return list(reversed(self._entries[-limit:]))

    def undo_last(self) -> dict[str, Any] | None:
        """The most recent undoable (rate/param) entry, removed from the log. The caller re-applies
        `old` and then calls record("undo", ...) itself, so the reversal shows up in the log too."""
        for i in range(len(self._entries) - 1, -1, -1):
            if self._entries[i]["kind"] in ("rate", "param"):
                return self._entries.pop(i)
        return None
