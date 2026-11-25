"""Domain operations shared by the API and the worker."""

import hashlib
import inspect
import json
import uuid
from datetime import datetime
from pathlib import Path
from typing import Any

from sqlalchemy import func, select
from sqlalchemy.engine import Engine
from sqlalchemy.orm import Session

from finforge.config import BacktestConfig
from finforge.data import Dataset as DataDir
from finforge.server import queue
from finforge.server.db import Dataset, Instrument, Job, Run, RunMetrics, StrategyRow
from finforge.strategies import BUILTIN

METRIC_FIELDS = (
    "total_return",
    "cagr",
    "volatility",
    "sharpe",
    "sortino",
    "max_drawdown",
    "var",
    "cvar",
)


def sync_strategies(session: Session) -> None:
    """Upsert the built-in strategy registry (D8: the API runs only these)."""
    for name, cls in BUILTIN.items():
        src = inspect.getsourcefile(cls)
        sha = hashlib.sha256(Path(src).read_bytes()).hexdigest() if src else ""
        row = session.get(StrategyRow, name) or StrategyRow(name=name)
        row.description = inspect.cleandoc(cls.__doc__ or "")
        row.params_schema = cls.params_schema
        row.defaults = cls.defaults
        row.source_sha256 = sha
        session.merge(row)
    session.commit()


def register_dataset(session: Session, path: Path) -> Dataset:
    """Register a dataset directory (finforge.data manifest). Idempotent by content id."""
    ds = DataDir(path)
    existing = session.get(Dataset, ds.id)
    if existing is not None:
        return existing
    m = ds.manifest
    row = Dataset(
        id=ds.id,
        name=m["name"],
        path=str(Path(path).resolve()),
        schema_version=m["schema_version"],
        time_first_ns=m["time_range_ns"]["first"],
        time_last_emit_ns=m["time_range_ns"]["last_emit"],
        files=m["files"],
        validation=m["validation"],
        manifest=m,
    )
    session.add(row)
    for i in m["instruments"]:
        session.add(
            Instrument(
                dataset_id=ds.id,
                **{
                    k: i[k]
                    for k in (
                        "instrument_id",
                        "symbol",
                        "asset_class",
                        "quote_currency",
                        "tick_size",
                        "lot_size",
                        "min_qty",
                        "contract_multiplier",
                    )
                },
            )
        )
    session.commit()
    return row


def create_run(session: Session, config: BacktestConfig, dataset_id: str, max_attempts: int) -> Run:
    run = Run(
        id=str(uuid.uuid4()),
        status="queued",
        dataset_id=dataset_id,
        strategy=config.strategy.name,
        config=config.to_json_dict(),
    )
    session.add(run)
    session.flush()
    session.add(
        Job(
            run_id=run.id,
            status="queued",
            attempts=0,
            max_attempts=max_attempts,
            progress=0.0,
            events_processed=0,
            cancel_requested=False,
        )
    )
    session.commit()
    return run


def finalize(
    engine: Engine, job_id: int, worker_id: str, run_id: str, run_dir: Path, child_error: str | None
) -> str | None:
    """Record a finished attempt: run status, hashes, timings, metrics, job status — all in one
    transaction, and only if the job is still this worker's. Returns the run status written."""
    manifest_path = run_dir / "manifest.json"
    m = json.loads(manifest_path.read_text()) if manifest_path.exists() else None
    if m is None:
        status, job_status = "failed", "failed"
        error = {"type": "WorkerChildDied", "message": child_error or "no manifest written"}
    else:
        status = m["status"]
        job_status = {"completed": "done", "cancelled": "cancelled"}.get(status, "failed")
        error = m["error"]
    with Session(engine) as s, s.begin():
        if not queue.finish(
            s.connection(),
            job_id,
            worker_id,
            job_status,
            None if error is None else error["message"],
        ):
            return None  # the lease was lost (the job was reaped): leave the run alone
        run = s.get(Run, run_id)
        run.status = status
        run.error = error
        run.finished_at = func.now()
        run.artefact_dir = str(run_dir)
        if m is not None:
            job = s.get(Job, job_id)
            job.events_processed = m["counts"]["events_processed"]  # exact, from the run itself
            run.strategy_hash = m["strategy"]["hash"]
            run.journal_hash = m["journal_hash"]
            run.engine_version = m["engine"]["version"]
            run.wall_seconds = m["timings"]["wall_seconds"]
            run.partial = m["partial"]
            run.reconciles = m["reconciles"]
        report_path = run_dir / "report.json"
        if status == "completed" and report_path.exists():
            rep = json.loads(report_path.read_text())
            perf, t = rep["performance"], rep["trades"]
            bench = rep.get("benchmark") or {}
            s.merge(
                RunMetrics(
                    run_id=run_id,
                    **{k: perf.get(k) for k in METRIC_FIELDS},
                    trades=t.get("count"),
                    win_rate=t.get("win_rate"),
                    profit_factor=t.get("profit_factor"),
                    fees=m["summary"]["total_fees"],
                    benchmark_return=(bench.get("performance") or {}).get("total_return"),
                )
            )
    return status


def progress_fraction(config: dict[str, Any], dataset: Dataset, sim_now: int) -> float:
    start = _ns(config.get("start")) or dataset.time_first_ns or 0
    end = _ns(config.get("end")) or dataset.time_last_emit_ns or start
    if end <= start:
        return 0.0
    return min(1.0, max(0.0, (sim_now - start) / (end - start)))


def _ns(t: str | None) -> int | None:
    if not t:
        return None
    return BacktestConfig._ns(datetime.fromisoformat(t.replace("Z", "+00:00")))


def dataset_of(session: Session, dataset_id: str) -> Dataset | None:
    return session.execute(select(Dataset).where(Dataset.id == dataset_id)).scalar_one_or_none()
