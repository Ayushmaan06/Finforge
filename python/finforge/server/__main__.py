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
    p = sub.add_parser("demo", help="create and register the synthetic 1-minute demo dataset")
    p.add_argument("--days", type=int, default=365)
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
    elif a.cmd == "demo":
        from finforge.data.demo import make_demo_dataset
        from finforge.server import service
        from finforge.server.db import make_engine, session_factory

        settings.ensure_dirs()
        out = settings.datasets_dir / f"demo-{a.days}d"
        if not (out / "manifest.json").exists():  # idempotent: a restarted stack reuses it
            make_demo_dataset(out, days=a.days)
        with session_factory(make_engine(settings.database_url))() as s:
            ds = service.register_dataset(s, out)  # idempotent by content id
            print(f"demo dataset {ds.id} ({ds.name})")
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
