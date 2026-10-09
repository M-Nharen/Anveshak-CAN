"""python -m anv_dashboard : serve the dashboard (set ANV_DASH_* variables, see settings.py)."""

from __future__ import annotations

import uvicorn

from .app import create_app
from .settings import Settings


def main() -> None:
    settings = Settings.from_env()
    uvicorn.run(
        create_app(settings), host=settings.host, port=settings.port, log_level="info"
    )


if __name__ == "__main__":
    main()
