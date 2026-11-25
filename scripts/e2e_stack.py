#!/usr/bin/env python3
"""Bring up the backend for the dashboard's end-to-end test (or a local demo) in one process:

    PostgreSQL (FINFORGE_DATABASE_URL, else a throwaway pgserver instance)
    → migrations → one-year synthetic demo dataset, registered → worker thread → API (uvicorn).

    python scripts/e2e_stack.py --port 8765 --storage /tmp/finforge-e2e

Stops everything on SIGTERM / Ctrl-C. Playwright starts it through `webServer` (frontend/
playwright.config.ts) and waits for /health.
"""

import argparse
import logging
import os
import signal
import tempfile
import threading
from pathlib import Path

from sqlalchemy import create_engine, text


def database_url() -> tuple[str, object | None]:
    url = os.environ.get("FINFORGE_DATABASE_URL")
    if url:
        return url, None
    import pgserver  # local development without Docker

    d = tempfile.mkdtemp(prefix="ffe2e")
    srv = pgserver.get_server(d, cleanup_mode="stop")
    host = srv.get_uri().split("host=")[1]
    admin = create_engine(
        f"postgresql+psycopg://postgres@/postgres?host={host}", isolation_level="AUTOCOMMIT"
    )
    with admin.connect() as c:
        c.execute(text("CREATE DATABASE finforge"))
    admin.dispose()
    return f"postgresql+psycopg://postgres@/finforge?host={host}", srv


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--storage", default=str(Path(tempfile.gettempdir()) / "finforge-e2e"))
    ap.add_argument("--days", type=int, default=365)
    ap.add_argument(
        "--cors",
        default="http://127.0.0.1:3100,http://localhost:3100,"
        "http://127.0.0.1:3000,http://localhost:3000",
    )
    a = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")

    url, server = database_url()
    os.environ.update(
        {
            "FINFORGE_DATABASE_URL": url,
            "FINFORGE_STORAGE_DIR": a.storage,
            "FINFORGE_CORS_ORIGINS": a.cors,
            "FINFORGE_HEARTBEAT_SECONDS": "1",
            "FINFORGE_POLL_SECONDS": "0.2",
        }
    )

    import uvicorn

    from finforge.data.demo import make_demo_dataset
    from finforge.server import service
    from finforge.server.api import create_app
    from finforge.server.db import make_engine, migrate, session_factory
    from finforge.server.settings import Settings
    from finforge.server.worker import Worker

    settings = Settings()
    settings.ensure_dirs()
    migrate(url)
    demo = make_demo_dataset(settings.datasets_dir / "demo", days=a.days)
    with session_factory(make_engine(url))() as s:
        service.sync_strategies(s)
        ds = service.register_dataset(s, demo)
        logging.info("demo dataset %s registered", ds.id)

    # uvicorn shuts down on SIGTERM, then re-raises it to the previous handler; make that handler
    # raise SystemExit so the `finally` below runs (worker stop, PostgreSQL cleanup).
    def _exit(*_: object) -> None:
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, _exit)
    stop = threading.Event()
    worker = threading.Thread(target=Worker(settings).run_forever, args=(stop,), daemon=True)
    worker.start()
    try:
        # uvicorn returns on SIGTERM / SIGINT (Playwright sends SIGTERM, see gracefulShutdown).
        uvicorn.run(create_app(settings), host="127.0.0.1", port=a.port, log_level="warning")
    finally:
        stop.set()
        worker.join(timeout=30)
        if server is not None:
            server.cleanup()  # stop the throwaway PostgreSQL instance


if __name__ == "__main__":
    main()
