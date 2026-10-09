"""Login for anything that moves hardware. Viewing needs no login."""

from __future__ import annotations

import hmac
import secrets
import time
from collections.abc import Callable


class Sessions:
    def __init__(
        self,
        password: str | None,
        ttl_s: float,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.generated = password is None
        self._password = password if password is not None else secrets.token_urlsafe(9)
        self._ttl = ttl_s
        self._clock = clock
        self._tokens: dict[str, float] = {}  # token -> expiry
        self._fails: list[float] = []

    @property
    def password(self) -> str:
        return self._password

    def login(self, candidate: str) -> str | None:
        """A new token, or None for a wrong password or too many recent failures."""
        now = self._clock()
        self._fails = [t for t in self._fails if now - t < 60.0]
        if len(self._fails) >= 10:
            return None
        if not hmac.compare_digest(candidate.encode(), self._password.encode()):
            self._fails.append(now)
            return None
        token = secrets.token_urlsafe(24)
        self._tokens[token] = now + self._ttl
        return token

    def valid(self, token: str | None) -> bool:
        if not token:
            return False
        expiry = self._tokens.get(token)
        if expiry is None:
            return False
        if self._clock() > expiry:
            del self._tokens[token]
            return False
        return True

    def logout(self, token: str) -> None:
        self._tokens.pop(token, None)
