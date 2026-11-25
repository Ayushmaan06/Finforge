"""HTTP API (PLAN Phase 7 task 4). OpenAPI docs at /docs.

Trust boundary (task 5): request bodies are validated with `extra="forbid"`; clients never send
filesystem paths (uploads and datasets are referred to by id, storage paths are generated here);
uploads are size-capped and sniffed by content; only built-in strategies run (D8) and their
parameters are validated against the strategy's JSON Schema before a run is queued.
"""

import json
import shutil
import time
import uuid
from collections.abc import Iterator
from contextlib import asynccontextmanager
from datetime import datetime
from decimal import Decimal
from pathlib import Path
from typing import Annotated, Any, Literal

import jsonschema
import numpy as np
import pyarrow.parquet as pq
from fastapi import Depends, FastAPI, File, HTTPException, Query, Request, UploadFile
from fastapi.responses import JSONResponse, StreamingResponse
from pydantic import BaseModel, ConfigDict, Field, ValidationError
from sqlalchemy import select
from sqlalchemy.orm import Session

from finforge import _engine
from finforge.analytics.compare import compare as compare_runs
from finforge.config import BacktestConfig, Execution, Fees, Risk
from finforge.data import DatasetRejected, ingest
from finforge.server import queue, service, storage
from finforge.server.db import (
    Dataset,
    Instrument,
    Job,
    Run,
    RunMetrics,
    StrategyRow,
    Upload,
    make_engine,
    session_factory,
)
from finforge.server.settings import Settings
from finforge.strategies import BUILTIN

SYMBOL = r"^[A-Za-z0-9._-]{1,64}$"
DECIMAL = r"^[0-9]+(\.[0-9]+)?$"
TERMINAL = ("completed", "failed", "cancelled")


class _In(BaseModel):
    model_config = ConfigDict(extra="forbid")


# --- Request models ------------------------------------------------------------------------------


class InstrumentIn(_In):
    symbol: str = Field(pattern=SYMBOL)
    asset_class: str = Field(default="spot", pattern=r"^[a-z_]{1,32}$")
    quote_currency: str = Field(pattern=r"^[A-Z0-9]{1,16}$")
    tick_size: str = Field(pattern=DECIMAL, max_length=32)
    lot_size: str = Field(pattern=DECIMAL, max_length=32)
    min_qty: int = Field(default=1, ge=1)
    contract_multiplier: int = Field(default=1, ge=1)


class SourceIn(_In):
    format: Literal["binance_klines", "binance_bookticker", "generic"] = "generic"
    kind: Literal["bars", "quotes", "trades"] | None = None
    symbol: str = Field(pattern=SYMBOL)
    uploads: list[uuid.UUID] = Field(min_length=1, max_length=1000)
    interval: str | None = Field(default=None, pattern=r"^[1-9][0-9]*[smhd]$")
    columns: dict[str, str] | None = None
    ts_unit: Literal["auto", "s", "ms", "us", "ns"] | None = None
    close_time: Literal["inclusive", "exclusive"] | None = None
    aggressor: Literal["side", "is_buyer_maker"] | None = None
    max_gap: str | None = Field(default=None, pattern=r"^[1-9][0-9]*[smhd]$")


class DatasetIn(_In):
    name: str = Field(pattern=r"^[A-Za-z0-9._ -]{1,120}$")
    instruments: list[InstrumentIn] = Field(min_length=1, max_length=1000)
    sources: list[SourceIn] = Field(min_length=1, max_length=1000)
    policies: dict[Literal["bars", "quotes", "trades"], dict[str, str]] = {}
    notes: list[str] = Field(default=[], max_length=20)
    chunk_rows: int = Field(default=65_536, ge=1_024, le=1_048_576)


class StrategyIn(_In):
    name: str
    params: dict[str, Any] = {}


class RunIn(_In):
    dataset_id: str = Field(pattern=r"^[0-9a-f]{64}$")
    universe: list[str] = Field(min_length=1, max_length=100)
    start: datetime | None = None
    end: datetime | None = None
    initial_capital: Decimal = Field(gt=0)
    execution: Execution = Execution()
    fees: Fees = Fees()
    risk: Risk = Risk()
    seed: int = 0
    strategy: StrategyIn
    benchmark: bool = True
    equity_interval_s: int | None = Field(default=None, ge=1)


# --- App -----------------------------------------------------------------------------------------


def create_app(settings: Settings | None = None) -> FastAPI:
    settings = settings or Settings()
    engine = make_engine(settings.database_url)
    sessions = session_factory(engine)

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        settings.ensure_dirs()
        with sessions() as s:
            service.sync_strategies(s)
        queue.reap(engine)
        yield
        engine.dispose()

    app = FastAPI(
        title="FinForge API",
        version=_engine.engine_version().split("+")[0],
        description="Market simulation and backtesting: datasets, built-in strategies, runs as "
        "background jobs, results and comparison. Units: timestamps are ns since the Unix epoch "
        "(UTC); money and prices in reports are in the quote currency.",
        lifespan=lifespan,
    )
    app.state.settings = settings
    app.state.engine = engine

    def db() -> Iterator[Session]:
        with sessions() as s:
            yield s

    DB = Annotated[Session, Depends(db)]

    @app.exception_handler(ValidationError)
    async def _pydantic(_: Request, exc: ValidationError) -> JSONResponse:
        return JSONResponse(status_code=422, content={"detail": json.loads(exc.json())})

    # --- health ---

    @app.get("/health", tags=["meta"])
    def health(session: DB) -> dict[str, Any]:
        session.execute(select(1))
        return {"status": "ok", "engine": _engine.engine_version()}

    # --- uploads ---

    @app.post(
        "/uploads",
        status_code=201,
        tags=["datasets"],
        summary="Upload one raw data file (CSV, single-file zip, Parquet or JSON).",
    )
    def upload(session: DB, file: Annotated[UploadFile, File()]) -> dict[str, Any]:
        try:
            uid, path, size, sha, kind = storage.save_upload(
                file.file, settings.uploads_dir, settings.max_upload_bytes
            )
        except storage.UploadTooLarge as e:
            raise HTTPException(413, str(e)) from None
        except storage.UnsupportedContent as e:
            raise HTTPException(415, str(e)) from None
        row = Upload(
            id=uid,
            original_name=storage.sanitise_filename(file.filename),
            stored_path=str(path),
            size=size,
            sha256=sha,
            content_type=kind,
        )
        session.add(row)
        session.commit()
        return _upload_out(row)

    @app.get("/uploads/{upload_id}", tags=["datasets"])
    def get_upload(upload_id: uuid.UUID, session: DB) -> dict[str, Any]:
        row = session.get(Upload, str(upload_id))
        if row is None:
            raise HTTPException(404, "unknown upload")
        return _upload_out(row)

    # --- datasets ---

    def spec_from(body: DatasetIn, session: Session) -> dict[str, Any]:
        """The ingest spec, with upload ids replaced by the server's own stored paths."""
        sources = []
        for src in body.sources:
            paths = []
            for uid in src.uploads:
                up = session.get(Upload, str(uid))
                if up is None:
                    raise HTTPException(422, f"unknown upload {uid}")
                if src.format != "generic" and up.content_type not in ("csv", "zip"):
                    raise HTTPException(
                        422, f"{src.format} needs CSV or zip uploads, not {up.content_type}"
                    )
                paths.append(up.stored_path)
            d = src.model_dump(exclude_none=True, exclude={"uploads"})
            d["paths"] = paths
            sources.append(d)
        return {
            "name": body.name,
            "notes": body.notes,
            "instruments": [i.model_dump() for i in body.instruments],
            "sources": sources,
            "policies": body.policies,
            "chunk_rows": body.chunk_rows,
        }

    @app.post(
        "/datasets/validate",
        tags=["datasets"],
        summary="Validate uploads against a dataset spec without storing anything.",
    )
    def validate_dataset(body: DatasetIn, session: DB) -> dict[str, Any]:
        spec = spec_from(body, session)
        try:
            report = ingest(spec, None, root="/").report
        except DatasetRejected as e:
            report = e.report
        except (ValueError, KeyError) as e:
            raise HTTPException(422, str(e)) from None
        return report.to_dict()

    @app.post(
        "/datasets",
        status_code=201,
        tags=["datasets"],
        summary="Ingest uploads into a versioned dataset and register it.",
    )
    def create_dataset(body: DatasetIn, session: DB) -> dict[str, Any]:
        spec = spec_from(body, session)
        tmp = settings.datasets_dir / f".ingest-{uuid.uuid4()}"
        try:
            result = ingest(spec, tmp, root="/")
        except DatasetRejected as e:
            raise HTTPException(422, {"message": str(e), "report": e.report.to_dict()}) from None
        except (ValueError, KeyError) as e:
            raise HTTPException(422, str(e)) from None
        final = settings.datasets_dir / result.manifest["id"]
        if final.exists():
            shutil.rmtree(tmp)  # same content already stored (ids are content hashes)
        else:
            tmp.rename(final)
        row = service.register_dataset(session, final)
        return _dataset_out(row, session)

    @app.get("/datasets", tags=["datasets"])
    def list_datasets(session: DB) -> list[dict[str, Any]]:
        rows = session.execute(select(Dataset).order_by(Dataset.created_at)).scalars()
        return [_dataset_out(r, session, brief=True) for r in rows]

    @app.get("/datasets/{dataset_id}", tags=["datasets"])
    def get_dataset(dataset_id: str, session: DB) -> dict[str, Any]:
        row = session.get(Dataset, dataset_id)
        if row is None:
            raise HTTPException(404, "unknown dataset")
        return _dataset_out(row, session)

    # --- strategies ---

    @app.get("/strategies", tags=["strategies"])
    def list_strategies(session: DB) -> list[dict[str, Any]]:
        rows = session.execute(select(StrategyRow).order_by(StrategyRow.name)).scalars()
        return [{"name": r.name, "summary": r.description.split("\n", 1)[0]} for r in rows]

    @app.get("/strategies/{name}", tags=["strategies"])
    def get_strategy(name: str, session: DB) -> dict[str, Any]:
        r = session.get(StrategyRow, name)
        if r is None:
            raise HTTPException(404, "unknown strategy")
        return {
            "name": r.name,
            "description": r.description,
            "params_schema": r.params_schema,
            "defaults": r.defaults,
            "source_sha256": r.source_sha256,
        }

    # --- runs ---

    @app.post(
        "/runs",
        status_code=202,
        tags=["runs"],
        summary="Queue a backtest of a built-in strategy on a registered dataset.",
    )
    def submit_run(body: RunIn, session: DB) -> dict[str, Any]:
        cls = BUILTIN.get(body.strategy.name)
        if cls is None:  # D8: built-in strategies only; never "module:Class" or file paths
            raise HTTPException(422, f"unknown strategy {body.strategy.name!r}")
        try:
            jsonschema.validate(body.strategy.params, cls.params_schema)
            cls(**body.strategy.params)  # the strategy's own cross-field checks
        except jsonschema.ValidationError as e:
            where = "/".join(map(str, e.absolute_path)) or "params"
            raise HTTPException(422, f"strategy params: {where}: {e.message}") from None
        except (ValueError, TypeError) as e:
            raise HTTPException(422, f"strategy params: {e}") from None
        ds = session.get(Dataset, body.dataset_id)
        if ds is None:
            raise HTTPException(404, "unknown dataset")
        symbols = {
            i.symbol
            for i in session.execute(
                select(Instrument).where(Instrument.dataset_id == ds.id)
            ).scalars()
        }
        missing = [u for u in body.universe if u not in symbols]
        if missing:
            raise HTTPException(422, f"symbols not in the dataset: {missing}")
        kind = "bars" if body.execution.model == "candle" else "quotes"
        have = {(f["kind"], f["symbol"]) for f in ds.files}
        lacking = [u for u in body.universe if (kind, u) not in have]
        if lacking:
            raise HTTPException(422, f"no {kind} data for {lacking}")
        cfg = body.model_dump(mode="json", exclude={"dataset_id"})
        cfg.update({"dataset": ds.path, "dataset_id": ds.id, "output_dir": str(settings.runs_dir)})
        try:
            config = BacktestConfig.model_validate(cfg)
        except ValidationError as e:
            raise HTTPException(422, json.loads(e.json())) from None
        run = service.create_run(session, config, ds.id, settings.max_attempts)
        return {"id": run.id, "status": run.status}

    @app.get("/runs", tags=["runs"])
    def list_runs(
        session: DB,
        status_: Annotated[str | None, Query(alias="status")] = None,
        limit: Annotated[int, Query(ge=1, le=500)] = 100,
    ) -> list[dict[str, Any]]:
        q = select(Run).order_by(Run.created_at.desc()).limit(limit)
        if status_:
            q = q.where(Run.status == status_)
        return [_run_out(r, session) for r in session.execute(q).scalars()]

    @app.get("/runs/{run_id}", tags=["runs"])
    def get_run(run_id: uuid.UUID, session: DB) -> dict[str, Any]:
        return _run_out(_run(session, run_id), session)

    @app.post("/runs/{run_id}/cancel", status_code=202, tags=["runs"])
    def cancel_run(run_id: uuid.UUID, session: DB) -> dict[str, Any]:
        run = _run(session, run_id)
        outcome = queue.request_cancel(engine, run.id)
        if outcome is None:
            raise HTTPException(409, f"run is already {run.status}")
        return {"id": run.id, "cancel": outcome}

    @app.get("/runs/{run_id}/results", tags=["runs"], summary="The run's report.json.")
    def results(run_id: uuid.UUID, session: DB) -> dict[str, Any]:
        run = _run(session, run_id)
        path = _artefact(run, "report.json")
        return json.loads(path.read_text())

    @app.get("/runs/{run_id}/equity", tags=["runs"])
    def equity(
        run_id: uuid.UUID, session: DB, max_points: Annotated[int, Query(ge=2, le=100_000)] = 2_000
    ) -> dict[str, Any]:
        run = _run(session, run_id)
        t = pq.read_table(_artefact(run, "equity.parquet"), columns=["ts", "equity_real"])
        ts, eq = t.column("ts").to_numpy(), t.column("equity_real").to_numpy()
        if len(ts) > max_points:  # even stride, always keeping the last point
            idx = np.unique(np.r_[np.linspace(0, len(ts) - 1, max_points).astype(int), len(ts) - 1])
            ts, eq = ts[idx], eq[idx]
        peak = np.maximum.accumulate(eq) if len(eq) else eq
        return {
            "ts": ts.tolist(),
            "equity": eq.tolist(),
            "drawdown": (eq / peak - 1.0).tolist() if len(eq) else [],
        }

    @app.get("/runs/{run_id}/trades", tags=["runs"])
    def trades(
        run_id: uuid.UUID,
        session: DB,
        offset: Annotated[int, Query(ge=0)] = 0,
        limit: Annotated[int, Query(ge=1, le=10_000)] = 500,
    ) -> dict[str, Any]:
        run = _run(session, run_id)
        t = pq.read_table(_artefact(run, "trades.parquet"))
        return {"total": t.num_rows, "offset": offset, "trades": t.slice(offset, limit).to_pylist()}

    @app.get(
        "/runs/{run_id}/events",
        tags=["runs"],
        summary="Server-sent events: progress and status until the run is finished.",
    )
    def events(
        run_id: uuid.UUID, session: DB, poll: Annotated[float, Query(ge=0.05, le=10)] = 0.5
    ) -> StreamingResponse:
        rid = _run(session, run_id).id

        def stream() -> Iterator[str]:
            last: tuple | None = None
            last_send = time.monotonic()
            while True:
                with sessions() as s:
                    run = s.get(Run, rid)
                    job = s.execute(select(Job).where(Job.run_id == rid)).scalar_one()
                    state = (run.status, round(job.progress, 4), job.attempts)
                    if state != last:
                        payload = {
                            "run": rid,
                            "status": run.status,
                            "progress": job.progress,
                            "events_processed": job.events_processed,
                            "sim_time_ns": job.sim_time_ns,
                            "attempt": job.attempts,
                        }
                        kind = "status" if last is None or last[0] != run.status else "progress"
                        yield f"event: {kind}\ndata: {json.dumps(payload)}\n\n"
                        last, last_send = state, time.monotonic()
                    if run.status in TERMINAL:
                        end = json.dumps({"run": rid, "status": run.status})
                        yield f"event: end\ndata: {end}\n\n"
                        return
                if time.monotonic() - last_send > 15:
                    yield ": keep-alive\n\n"
                    last_send = time.monotonic()
                time.sleep(poll)

        return StreamingResponse(
            stream(), media_type="text/event-stream", headers={"Cache-Control": "no-cache"}
        )

    @app.get(
        "/compare",
        tags=["runs"],
        summary="Metrics table and aligned daily equity/drawdown for completed runs.",
    )
    def compare(
        session: DB, runs: Annotated[str, Query(description="comma-separated run ids")]
    ) -> dict[str, Any]:
        ids = [r.strip() for r in runs.split(",") if r.strip()]
        if not 1 <= len(ids) <= 20:
            raise HTTPException(422, "give between 1 and 20 run ids")
        dirs = []
        for rid in ids:
            try:
                run = _run(session, uuid.UUID(rid))
            except ValueError:
                raise HTTPException(422, f"bad run id {rid!r}") from None
            if run.status != "completed":
                raise HTTPException(409, f"run {rid} is {run.status}")
            dirs.append(Path(run.artefact_dir))
        c = compare_runs(dirs)
        for row, rid in zip(c.rows, ids, strict=True):
            row["run_id"] = rid
            row.pop("run_dir", None)
        return {"rows": c.rows, "series": c.series.to_pydict()}

    # --- helpers ---

    def _run(session: Session, run_id: uuid.UUID) -> Run:
        run = session.get(Run, str(run_id))
        if run is None:
            raise HTTPException(404, "unknown run")
        return run

    def _artefact(run: Run, name: str) -> Path:
        if run.artefact_dir is None or not (Path(run.artefact_dir) / name).exists():
            raise HTTPException(
                404 if run.status in TERMINAL else 409,
                f"{name} is not available (run is {run.status})",
            )
        return Path(run.artefact_dir) / name

    return app


def _upload_out(row: Upload) -> dict[str, Any]:
    return {
        "id": row.id,
        "filename": row.original_name,
        "size": row.size,
        "sha256": row.sha256,
        "content_type": row.content_type,
        "created_at": row.created_at.isoformat(),
    }


def _dataset_out(row: Dataset, session: Session, brief: bool = False) -> dict[str, Any]:
    out = {
        "id": row.id,
        "name": row.name,
        "schema_version": row.schema_version,
        "time_first_ns": row.time_first_ns,
        "time_last_emit_ns": row.time_last_emit_ns,
        "validation_status": row.validation["status"],
        "files": [{k: f[k] for k in ("kind", "symbol", "rows")} for f in row.files],
        "created_at": row.created_at.isoformat(),
    }
    if not brief:
        out["instruments"] = [
            {
                c: getattr(i, c)
                for c in (
                    "instrument_id",
                    "symbol",
                    "asset_class",
                    "quote_currency",
                    "tick_size",
                    "lot_size",
                    "min_qty",
                    "contract_multiplier",
                )
            }
            for i in session.execute(
                select(Instrument)
                .where(Instrument.dataset_id == row.id)
                .order_by(Instrument.instrument_id)
            ).scalars()
        ]
        out["validation"] = row.validation
    return out


def _run_out(run: Run, session: Session) -> dict[str, Any]:
    job = session.execute(select(Job).where(Job.run_id == run.id)).scalar_one()
    m = session.get(RunMetrics, run.id)
    return {
        "id": run.id,
        "status": run.status,
        "dataset_id": run.dataset_id,
        "strategy": run.strategy,
        "params": run.config["strategy"]["params"],
        "config": run.config,
        "created_at": run.created_at.isoformat(),
        "started_at": run.started_at.isoformat() if run.started_at else None,
        "finished_at": run.finished_at.isoformat() if run.finished_at else None,
        "wall_seconds": run.wall_seconds,
        "journal_hash": run.journal_hash,
        "strategy_hash": run.strategy_hash,
        "engine_version": run.engine_version,
        "reconciles": run.reconciles,
        "partial": run.partial,
        "error": run.error,
        "job": {
            "status": job.status,
            "attempts": job.attempts,
            "max_attempts": job.max_attempts,
            "progress": job.progress,
            "events_processed": job.events_processed,
            "cancel_requested": job.cancel_requested,
            "last_error": job.last_error,
        },
        "metrics": None
        if m is None
        else {
            c: getattr(m, c)
            for c in (
                *service.METRIC_FIELDS,
                "trades",
                "win_rate",
                "profit_factor",
                "fees",
                "benchmark_return",
            )
        },
    }
