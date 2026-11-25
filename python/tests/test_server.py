"""API, persistence and job queue against a real PostgreSQL (PLAN Phase 7 exit criteria).

The database comes from FINFORGE_TEST_DATABASE_URL (CI: a postgres service container) or, locally,
from `pgserver` (PostgreSQL binaries in a pip package). Every test gets a fresh, migrated database.
"""

import io
import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from pathlib import Path

import numpy as np
import pytest

pytest.importorskip("fastapi")
from fastapi.testclient import TestClient
from sqlalchemy import create_engine, text

from finforge.server import queue
from finforge.server.api import create_app
from finforge.server.db import make_engine, migrate
from finforge.server.settings import Settings
from finforge.server.worker import Worker

FIXTURES = Path(__file__).resolve().parents[2] / "datasets" / "fixtures"
REGISTRY = json.loads((FIXTURES / "registry.json").read_text())["instruments"]


@pytest.fixture(scope="session")
def pg_admin_url() -> str:
    url = os.environ.get("FINFORGE_TEST_DATABASE_URL")
    if url:
        return url
    if os.environ.get("FINFORGE_REQUIRE_DB"):  # CI: never skip these tests silently
        pytest.fail("FINFORGE_REQUIRE_DB is set but FINFORGE_TEST_DATABASE_URL is not")
    pgserver = pytest.importorskip(
        "pgserver", reason="needs FINFORGE_TEST_DATABASE_URL or pgserver"
    )
    d = tempfile.mkdtemp(prefix="ffpg")  # short path: unix socket names are limited
    srv = pgserver.get_server(d, cleanup_mode="stop")
    host = srv.get_uri().split("host=")[1]
    return f"postgresql+psycopg://postgres@/postgres?host={host}"


@pytest.fixture
def db_url(pg_admin_url: str) -> str:
    name = f"ff_{uuid.uuid4().hex[:12]}"
    admin = create_engine(pg_admin_url, isolation_level="AUTOCOMMIT")
    with admin.connect() as c:
        c.execute(text(f'CREATE DATABASE "{name}"'))
    base, _, query = pg_admin_url.partition("?")
    url = base.rsplit("/", 1)[0] + f"/{name}" + (f"?{query}" if query else "")
    migrate(url)
    yield url
    with admin.connect() as c:
        c.execute(text(f'DROP DATABASE IF EXISTS "{name}" WITH (FORCE)'))
    admin.dispose()


def make_settings(db_url: str, tmp_path: Path, **over) -> Settings:
    base = {
        "database_url": db_url,
        "storage_dir": tmp_path / "var",
        "lease_seconds": 3.0,
        "heartbeat_seconds": 0.3,
        "max_attempts": 1,
        "poll_seconds": 0.1,
        "progress_every": 1024,
    }
    base.update(over)
    return Settings(**base)


@pytest.fixture
def worker_thread():
    """Start an in-process worker loop (it still runs each backtest in a child process)."""
    stops: list[tuple[threading.Event, threading.Thread]] = []

    def start(settings: Settings) -> Worker:
        w = Worker(settings)
        stop = threading.Event()
        t = threading.Thread(target=w.run_forever, args=(stop,), daemon=True)
        t.start()
        stops.append((stop, t))
        return w

    yield start
    for stop, t in stops:
        stop.set()
        t.join(timeout=60)


def wait_for(fn, timeout: float = 60.0, every: float = 0.1):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        v = fn()
        if v:
            return v
        time.sleep(every)
    raise AssertionError("timed out waiting")


def upload(client: TestClient, path: Path, name: str | None = None) -> str:
    r = client.post("/uploads", files={"file": (name or path.name, path.read_bytes())})
    assert r.status_code == 201, r.text
    return r.json()["id"]


def kline_spec(uploads: dict[str, str]) -> dict:
    return {
        "name": "fixture klines",
        "instruments": REGISTRY,
        "sources": [
            {"format": "binance_klines", "symbol": s, "interval": "1m", "uploads": [u]}
            for s, u in uploads.items()
        ],
    }


# --- Exit criterion 1: the scripted HTTP workflow -------------------------------------------------


def test_scripted_workflow(db_url: str, tmp_path: Path, worker_thread) -> None:
    settings = make_settings(db_url, tmp_path)
    with TestClient(create_app(settings)) as client:
        # upload -> validate -> create dataset
        ups = {
            "AAA": upload(client, FIXTURES / "klines_AAA_ms.csv"),
            "BBB": upload(client, FIXTURES / "klines_BBB_ms.csv"),
        }
        rep = client.post("/datasets/validate", json=kline_spec(ups)).json()
        assert rep["status"] == "clean" and rep["rows_read"] == 600
        r = client.post("/datasets", json=kline_spec(ups))
        assert r.status_code == 201, r.text
        ds = r.json()
        assert [i["symbol"] for i in ds["instruments"]] == ["AAA", "BBB"]
        assert client.post("/datasets", json=kline_spec(ups)).json()["id"] == ds["id"]  # idempotent
        assert [d["id"] for d in client.get("/datasets").json()] == [ds["id"]]

        strategies = {s["name"] for s in client.get("/strategies").json()}
        assert {"ma_cross", "mean_reversion", "momentum", "pairs", "buy_and_hold"} <= strategies
        schema = client.get("/strategies/ma_cross").json()["params_schema"]
        assert schema["additionalProperties"] is False and "fast" in schema["properties"]

        # submit two runs -> stream progress -> results -> compare
        def submit(strategy: str, params: dict) -> str:
            body = {
                "dataset_id": ds["id"],
                "universe": ["AAA", "BBB"],
                "initial_capital": "100000",
                "fees": {"taker_bps": 10},
                "strategy": {"name": strategy, "params": params},
            }
            r = client.post("/runs", json=body)
            assert r.status_code == 202, r.text
            assert r.json()["status"] == "queued"
            return r.json()["id"]

        a = submit("ma_cross", {"fast": 5, "slow": 20, "qty": "1"})
        b = submit("momentum", {"lookback": 30, "rebalance": 10, "qty": "1"})
        assert client.get(f"/runs/{a}").json()["status"] == "queued"

        # Subscribe first, then start the worker, so the stream sees the whole life of the run.
        events = []
        with client.stream("GET", f"/runs/{a}/events", params={"poll": 0.05}) as stream:
            kind = None
            for line in stream.iter_lines():
                if line.startswith("event: "):
                    kind = line[7:]
                elif line.startswith("data: "):
                    events.append((kind, json.loads(line[6:])))
                    if len(events) == 1:
                        worker_thread(settings)
                    if kind == "end":
                        break
        statuses = [e["status"] for k, e in events if k == "status"]
        assert statuses == ["queued", "running", "completed"], events
        assert events[-1] == ("end", {"run": a, "status": "completed"})
        wait_for(lambda: client.get(f"/runs/{b}").json()["status"] == "completed")

        run = client.get(f"/runs/{a}").json()
        assert run["reconciles"] is True and run["job"]["status"] == "done"
        assert run["job"]["progress"] == 1.0 and len(run["journal_hash"]) == 64
        assert run["metrics"]["trades"] > 0 and run["metrics"]["benchmark_return"] is not None
        report = client.get(f"/runs/{a}/results").json()
        assert report["performance"]["total_return"] == pytest.approx(
            run["metrics"]["total_return"]
        )
        eq = client.get(f"/runs/{a}/equity", params={"max_points": 50}).json()
        assert len(eq["ts"]) <= 51 and eq["ts"] == sorted(eq["ts"])
        tr = client.get(f"/runs/{a}/trades", params={"limit": 3}).json()
        assert tr["total"] == run["metrics"]["trades"] and len(tr["trades"]) == 3
        cmp = client.get("/compare", params={"runs": f"{a},{b}"}).json()
        assert [row["run_id"] for row in cmp["rows"]] == [a, b]
        assert len(cmp["series"]) == 5  # ts + equity/drawdown for each run
        assert {r["id"] for r in client.get("/runs", params={"status": "completed"}).json()} == {
            a,
            b,
        }


# --- Exit criterion 4: OpenAPI --------------------------------------------------------------------


def test_openapi_docs_are_served(db_url: str, tmp_path: Path) -> None:
    with TestClient(create_app(make_settings(db_url, tmp_path))) as client:
        assert client.get("/docs").status_code == 200
        spec = client.get("/openapi.json").json()
        for path in (
            "/uploads",
            "/datasets",
            "/datasets/validate",
            "/strategies",
            "/runs",
            "/runs/{run_id}",
            "/runs/{run_id}/cancel",
            "/runs/{run_id}/events",
            "/runs/{run_id}/results",
            "/runs/{run_id}/equity",
            "/runs/{run_id}/trades",
            "/compare",
        ):
            assert path in spec["paths"], path
        assert client.get("/health").json()["status"] == "ok"


# --- Trust boundary (task 5) ----------------------------------------------------------------------


def test_trust_boundary(db_url: str, tmp_path: Path) -> None:
    settings = make_settings(db_url, tmp_path, max_upload_bytes=50_000)
    with TestClient(create_app(settings)) as client:
        big = io.BytesIO(b"a,b\n" + b"1,2\n" * 20_000)
        assert client.post("/uploads", files={"file": ("x.csv", big)}).status_code == 413
        assert (
            client.post("/uploads", files={"file": ("x.csv", b"\x00\x01\x02binary")}).status_code
            == 415
        )
        assert (
            client.post("/uploads", files={"file": ("x.csv", b"no commas here\n")}).status_code
            == 415
        )
        r = client.post("/uploads", files={"file": ("../../etc/passwd", b"a,b\n1,2\n")})
        assert r.status_code == 201
        up = r.json()
        assert up["filename"] == "passwd" and up["content_type"] == "csv"
        stored = list((settings.uploads_dir).iterdir())
        assert [p.name for p in stored] == [f"{up['id']}.csv"]  # server-generated name only
        # Leftover partial files are removed when an upload is refused.
        assert not any(p.name.startswith(".") for p in settings.uploads_dir.iterdir())

        spec = kline_spec({"AAA": up["id"]})
        spec["sources"][0]["paths"] = ["/etc/passwd"]  # clients cannot name files
        assert client.post("/datasets/validate", json=spec).status_code == 422
        spec = kline_spec({"AAA": str(uuid.uuid4())})
        assert client.post("/datasets/validate", json=spec).status_code == 422

        ok = upload(client, FIXTURES / "klines_AAA_ms.csv")
        ds = client.post("/datasets", json=kline_spec({"AAA": ok})).json()["id"]
        base = {"dataset_id": ds, "universe": ["AAA"], "initial_capital": "1000"}

        def run(**over) -> int:
            return client.post("/runs", json={**base, **over}).status_code

        assert run(strategy={"name": "os:system"}) == 422  # D8: built-ins only
        assert (
            run(strategy={"name": "python/finforge/strategies/ma_cross.py:MovingAverageCrossover"})
            == 422
        )
        assert run(strategy={"name": "ma_cross", "params": {"fast": -1}}) == 422  # schema
        assert run(strategy={"name": "ma_cross", "params": {"evil": 1}}) == 422  # no extra keys
        assert run(strategy={"name": "ma_cross", "params": {"fast": 50, "slow": 10}}) == 422
        assert run(strategy={"name": "ma_cross"}, output_dir="/tmp") == 422  # no paths
        assert run(strategy={"name": "ma_cross"}, universe=["BBB"]) == 422
        assert run(strategy={"name": "ma_cross"}, execution={"model": "quote"}) == 422  # no quotes
        assert (
            client.post(
                "/runs", json={**base, "dataset_id": "0" * 64, "strategy": {"name": "ma_cross"}}
            ).status_code
            == 404
        )
        assert run(strategy={"name": "ma_cross"}) == 202


# --- Long runs: cancel and worker loss ------------------------------------------------------------


@pytest.fixture(scope="module")
def long_dataset(tmp_path_factory) -> Path:
    """150 000 one-minute bars: a run with mean_reversion takes several seconds."""
    from finforge.data import ingest

    d = tmp_path_factory.mktemp("long")
    rng = np.random.default_rng(1)
    closes = 100 + np.cumsum(rng.integers(-2, 3, 150_000)) * 0.01
    closes = np.maximum(closes, 1.0)
    t0 = 1_709_251_200_000
    lines = ["time,o,h,l,c,v"] + [
        f"{t0 + i * 60_000},{c:.2f},{c:.2f},{c:.2f},{c:.2f},1" for i, c in enumerate(closes)
    ]
    (d / "aaa.csv").write_text("\n".join(lines) + "\n")
    out = d / "ds"
    ingest(
        {
            "name": "long",
            "instruments": REGISTRY[:1],
            "sources": [
                {
                    "format": "generic",
                    "kind": "bars",
                    "symbol": "AAA",
                    "interval": "1m",
                    "paths": ["aaa.csv"],
                    "columns": {
                        "ts_open": "time",
                        "open": "o",
                        "high": "h",
                        "low": "l",
                        "close": "c",
                        "volume": "v",
                    },
                }
            ],
        },
        out,
        root=d,
    )
    return out


def register(settings: Settings, path: Path) -> str:
    from finforge.server import service
    from finforge.server.db import session_factory

    with session_factory(make_engine(settings.database_url))() as s:
        service.sync_strategies(s)
        return service.register_dataset(s, path).id


LONG = {
    "name": "mean_reversion",
    "params": {"lookback": 300, "entry_z": 2.5, "exit_z": 0.5, "qty": "1"},
}


def test_cancelling_a_running_job(
    db_url: str, tmp_path: Path, long_dataset: Path, worker_thread
) -> None:
    settings = make_settings(db_url, tmp_path)
    ds = register(settings, long_dataset)
    with TestClient(create_app(settings)) as client:
        body = {
            "dataset_id": ds,
            "universe": ["AAA"],
            "initial_capital": "100000",
            "strategy": LONG,
            "benchmark": False,
        }
        queued = client.post("/runs", json=body).json()["id"]
        # A queued job is cancelled at once.
        assert client.post(f"/runs/{queued}/cancel").json()["cancel"] == "cancelled"
        assert client.get(f"/runs/{queued}").json()["status"] == "cancelled"

        rid = client.post("/runs", json=body).json()["id"]
        worker_thread(settings)
        wait_for(lambda: client.get(f"/runs/{rid}").json()["job"]["progress"] > 0)
        assert client.post(f"/runs/{rid}/cancel").json()["cancel"] == "requested"
        run = wait_for(
            lambda: (
                (r := client.get(f"/runs/{rid}").json())["status"]
                in ("completed", "failed", "cancelled")
                and r
            )
        )
        assert run["status"] == "cancelled" and run["job"]["status"] == "cancelled"
        assert run["partial"] is True and 0 < run["job"]["progress"] < 1
        manifest = json.loads(
            (Path(run["config"]["output_dir"]) / rid / "attempt-1" / "manifest.json").read_text()
        )
        assert manifest["status"] == "cancelled" and manifest["reconciles"]
        assert client.post(f"/runs/{rid}/cancel").status_code == 409  # already finished


def _worker_process(settings: Settings) -> subprocess.Popen:
    env = {
        **os.environ,
        "FINFORGE_DATABASE_URL": settings.database_url,
        "FINFORGE_STORAGE_DIR": str(settings.storage_dir),
        "FINFORGE_LEASE_SECONDS": str(settings.lease_seconds),
        "FINFORGE_HEARTBEAT_SECONDS": str(settings.heartbeat_seconds),
        "FINFORGE_POLL_SECONDS": "0.1",
        "FINFORGE_PROGRESS_EVERY": "1024",
    }
    return subprocess.Popen(
        [sys.executable, "-m", "finforge.server", "worker"],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


@pytest.mark.parametrize("max_attempts", [1, 2])
def test_killing_the_worker_fails_or_retries_the_job(
    db_url: str, tmp_path: Path, long_dataset: Path, max_attempts: int
) -> None:
    settings = make_settings(db_url, tmp_path, max_attempts=max_attempts)
    ds = register(settings, long_dataset)
    engine = make_engine(settings.database_url)
    with TestClient(create_app(settings)) as client:
        quick = client.post(
            "/runs",
            json={
                "dataset_id": ds,
                "universe": ["AAA"],
                "initial_capital": "100000",
                "start": "2024-03-01T00:00:00Z",
                "end": "2024-03-02T00:00:00Z",
                "strategy": {"name": "ma_cross", "params": {"fast": 5, "slow": 20}},
            },
        ).json()["id"]
        worker = _worker_process(settings)
        try:
            before = wait_for(
                lambda: (r := client.get(f"/runs/{quick}").json())["status"] == "completed" and r
            )
            files_before = {
                p.name: p.read_bytes()
                for p in Path(before["config"]["output_dir"]).joinpath(quick, "attempt-1").iterdir()
                if p.is_file()
            }

            rid = client.post(
                "/runs",
                json={
                    "dataset_id": ds,
                    "universe": ["AAA"],
                    "initial_capital": "100000",
                    "strategy": LONG,
                    "benchmark": False,
                },
            ).json()["id"]
            wait_for(lambda: client.get(f"/runs/{rid}").json()["job"]["progress"] > 0)
            worker.send_signal(signal.SIGKILL)  # the worker dies mid-run, no clean-up
            worker.wait()
        finally:
            if worker.poll() is None:
                worker.kill()
        assert client.get(f"/runs/{rid}").json()["status"] == "running"  # nobody knows yet
        time.sleep(settings.lease_seconds + 0.5)
        reaped = queue.reap(engine)
        expected = "failed" if max_attempts == 1 else "queued"
        assert [(r[1], r[2]) for r in reaped] == [(rid, expected)]
        run = client.get(f"/runs/{rid}").json()
        if max_attempts == 1:
            assert run["status"] == "failed" and run["error"]["type"] == "WorkerLost"
            assert run["job"]["status"] == "failed" and run["job"]["attempts"] == 1
        else:
            assert run["status"] == "queued" and run["job"]["attempts"] == 1
            Worker(settings).run_once()  # another worker picks it up
            run = client.get(f"/runs/{rid}").json()
            assert run["status"] == "completed" and run["job"]["attempts"] == 2
            assert run["reconciles"] is True
        # The earlier run is untouched: same row, same files.
        after = client.get(f"/runs/{quick}").json()
        assert {
            k: after[k] for k in ("status", "journal_hash", "finished_at", "metrics", "job")
        } == {k: before[k] for k in ("status", "journal_hash", "finished_at", "metrics", "job")}
        files_after = {
            p.name: p.read_bytes()
            for p in Path(after["config"]["output_dir"]).joinpath(quick, "attempt-1").iterdir()
            if p.is_file()
        }
        assert files_after == files_before
        # The orphaned backtest process noticed its parent died and stopped on its own.
        orphan = Path(after["config"]["output_dir"]) / rid / "attempt-1" / "manifest.json"
        m = wait_for(lambda: orphan.exists() and json.loads(orphan.read_text()), timeout=30)
        assert m["status"] == "cancelled"
    engine.dispose()
