"""`finforge server {migrate,api,worker}` (also `python -m finforge.server ...`).

Configuration comes from FINFORGE_* environment variables (finforge.server.settings).
"""

import argparse
import logging
import signal
import sys
import threading


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="finforge server")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("migrate", help="apply database migrations")
    p = sub.add_parser("api", help="serve the HTTP API (uvicorn)")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8000)
    p = sub.add_parser("worker", help="run backtest jobs")
    p.add_argument("--once", action="store_true", help="run at most one job, then exit")
    a = ap.parse_args(argv)

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")
    from finforge.server.settings import Settings

    settings = Settings()
    if a.cmd == "migrate":
        from finforge.server.db import migrate

        migrate(settings.database_url)
        print("migrated")
    elif a.cmd == "api":
        import uvicorn

        from finforge.server.api import create_app

        uvicorn.run(create_app(settings), host=a.host, port=a.port)
    else:
        from finforge.server.worker import Worker

        settings.ensure_dirs()
        w = Worker(settings)
        if a.once:
            w.run_once()
            return 0
        stop = threading.Event()
        signal.signal(signal.SIGTERM, lambda *_: stop.set())
        signal.signal(signal.SIGINT, lambda *_: stop.set())
        w.run_forever(stop)
    return 0


if __name__ == "__main__":
    sys.exit(main())
