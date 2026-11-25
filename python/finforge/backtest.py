"""End-to-end backtest (PLAN Phase 5 task 4): config → engine run → run directory.

    from finforge.backtest import run
    result = run("examples/ma_cross.yaml")
    result.status, result.journal_hash, result.run_dir

The run directory holds:

- `manifest.json`: config, dataset id, strategy hash, engine version and git sha, seed, timings,
  journal hash, status, reconciliation, artefact hashes; on failure the traceback and `partial`.
- `orders.parquet`, `fills.parquet`, `equity.parquet`: integer engine units (ticks, lots, Money)
  plus `*_real` float columns for analysis.

Determinism (DET-1): the same config gives the same journal hash and byte-identical Parquet files.
Wall-clock times appear only in the manifest.
"""

import hashlib
import inspect
import json
import time
import traceback
from collections.abc import Iterator
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

from finforge import _engine
from finforge.config import BacktestConfig
from finforge.data.dataset import Dataset
from finforge.sdk import MONEY_SCALE, Context, Strategy, _Runner, from_grid
from finforge.strategies import resolve

RUN_FORMAT_VERSION = 1
_ENUMS = _engine.enum_names()


@dataclass
class RunResult:
    run_dir: Path
    status: str  # "completed" | "failed"
    manifest: dict[str, Any]
    strategy: Strategy

    @property
    def journal_hash(self) -> str:
        return self.manifest["journal_hash"]

    @property
    def reconciles(self) -> bool:
        return bool(self.manifest["reconciles"])

    def read(self, name: str) -> pa.Table:
        """One artefact: "orders", "fills" or "equity"."""
        return pq.read_table(self.run_dir / f"{name}.parquet")


def _canonical(obj: Any) -> bytes:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), default=str).encode()


def _sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def strategy_hash(cls: type[Strategy], name: str, params: dict[str, Any]) -> dict[str, Any]:
    """D6: SHA-256 of the strategy's source file, plus its name and parameters."""
    source = Path(inspect.getsourcefile(cls) or "")
    src_sha = _sha256_file(source) if source.is_file() else "unknown"
    digest = hashlib.sha256(
        _canonical({"source": src_sha, "name": name, "params": params})
    ).hexdigest()
    return {
        "name": name,
        "class": f"{cls.__module__}.{cls.__qualname__}",
        "source_sha256": src_sha,
        "params": params,
        "hash": digest,
    }


def _engine_info() -> dict[str, str]:
    v = _engine.engine_version()
    semver, _, rest = v.partition("+")
    sha, _, build = rest.rpartition(".")
    return {"version": v, "semver": semver, "git_sha": sha, "build_type": build}


def _chunks(
    ds: Dataset, entry: dict[str, Any], start: int | None, end: int | None, chunk_rows: int
) -> Iterator[tuple[np.ndarray, ...]]:
    """Replay chunks, restricted to [start, end] by emit time when a window is set (copies only
    then; otherwise zero-copy views of the Arrow buffers)."""
    for cols in ds.iter_chunks(entry, chunk_rows):
        if start is None and end is None:
            yield cols
            continue
        open_ts = cols[0]  # bars: ts_open; quotes: ts
        emit = cols[1] if entry["kind"] == "bars" else cols[0]
        keep = np.ones(len(emit), dtype=bool)
        if start is not None:
            keep &= open_ts >= start
        if end is not None:
            keep &= emit <= end
        if keep.any():
            yield tuple(np.ascontiguousarray(c[keep]) for c in cols)


def _orders_table(engine: Any, ctx: Context, registry: Any) -> pa.Table:
    o = engine.orders()
    sym = np.array([i.symbol for i in registry], dtype=object)
    tick = [i.tick for i in registry]
    lot = [i.lot for i in registry]
    inst = o["instrument_id"]
    names = {k: np.array(v, dtype=object) for k, v in _ENUMS.items()}
    cols: dict[str, list] = {
        "order_id": list(o["order_id"]),
        "client_ref": list(o["client_ref"]),
        "symbol": list(sym[inst]) if len(inst) else [],
        "side": [s.lower() for s in names["side"][o["side"]]],
        "type": [s.lower() for s in names["type"][o["type"]]],
        "tif": [s.lower() for s in names["tif"][o["tif"]]],
        "price": list(o["price"]),
        "qty": list(o["qty"]),
        "filled": list(o["filled"]),
        "cancelled": list(o["cancelled"]),
        "status": list(names["status"][o["status"]]),
        "reject_reason": list(names["reject_reason"][o["reject_reason"]]),
        "cancel_reason": list(names["cancel_reason"][o["cancel_reason"]]),
        "submit_ts": list(o["submit_ts"]),
        "sent_ts": list(o["sent_ts"]),
    }
    cols["price_real"] = [from_grid(int(p), tick[i]) for p, i in zip(o["price"], inst, strict=True)]
    cols["qty_real"] = [from_grid(int(q), lot[i]) for q, i in zip(o["qty"], inst, strict=True)]
    # Orders rejected by the SDK (OFF_TICK / OFF_LOT) never reached the engine: order_id 0.
    for local in ctx._local:
        h = local.handle
        cols["order_id"].append(0)
        cols["client_ref"].append(0)
        cols["symbol"].append(h.symbol)
        cols["side"].append(h.side)
        cols["type"].append(h.type)
        cols["tif"].append(h.tif)
        for k in ("price", "qty", "filled", "cancelled"):
            cols[k].append(0)
        cols["status"].append("REJECTED")
        cols["reject_reason"].append(h.reject_reason)
        cols["cancel_reason"].append("NONE")
        cols["submit_ts"].append(local.ts)
        cols["sent_ts"].append(local.ts)
        cols["price_real"].append(h.price if h.price is not None else 0.0)
        cols["qty_real"].append(h.qty)
    schema = pa.schema(
        [
            ("order_id", pa.uint64()),
            ("client_ref", pa.uint64()),
            ("symbol", pa.string()),
            ("side", pa.string()),
            ("type", pa.string()),
            ("tif", pa.string()),
            ("price", pa.int64()),
            ("qty", pa.int64()),
            ("filled", pa.int64()),
            ("cancelled", pa.int64()),
            ("status", pa.string()),
            ("reject_reason", pa.string()),
            ("cancel_reason", pa.string()),
            ("submit_ts", pa.int64()),
            ("sent_ts", pa.int64()),
            ("price_real", pa.float64()),
            ("qty_real", pa.float64()),
        ]
    )
    return pa.table(
        {k: pa.array(cols[k], schema.field(k).type) for k in schema.names}, schema=schema
    )


def _fills_table(engine: Any, registry: Any) -> tuple[pa.Table, dict[str, int]]:
    f = engine.fills()
    sym = [i.symbol for i in registry]
    tick = [i.tick for i in registry]
    lot = [i.lot for i in registry]
    inst = f["instrument_id"]
    sides = _ENUMS["side"]
    table = pa.table(
        {
            "ts": pa.array(f["ts"], pa.int64()),
            "order_id": pa.array(f["order_id"], pa.uint64()),
            "trade_id": pa.array(f["trade_id"], pa.uint64()),
            "symbol": pa.array([sym[i] for i in inst], pa.string()),
            "side": pa.array([sides[s].lower() for s in f["side"]], pa.string()),
            "price": pa.array(f["price"], pa.int64()),
            "qty": pa.array(f["qty"], pa.int64()),
            "fee": pa.array(f["fee"], pa.int64()),
            "liquidity": pa.array(
                [_ENUMS["liquidity"][x].lower() for x in f["liquidity"]], pa.string()
            ),
            "price_real": pa.array(
                [from_grid(int(p), tick[i]) for p, i in zip(f["price"], inst, strict=True)],
                pa.float64(),
            ),
            "qty_real": pa.array(
                [from_grid(int(q), lot[i]) for q, i in zip(f["qty"], inst, strict=True)],
                pa.float64(),
            ),
            "fee_real": pa.array(f["fee"] / MONEY_SCALE, pa.float64()),
        }
    )
    borrow = {"charges": len(f["borrow_ts"]), "total": int(f["borrow_amount"].sum())}
    return table, borrow


def _equity_table(engine: Any) -> pa.Table:
    e = engine.equity()
    cols = {
        k: pa.array(e[k], pa.int64())
        for k in (
            "ts",
            "cash",
            "equity",
            "realised_pnl",
            "unrealised_pnl",
            "total_fees",
            "borrow_costs",
            "gross_exposure",
        )
    }
    cols["equity_real"] = pa.array(e["equity"] / MONEY_SCALE, pa.float64())
    return pa.table(cols)


def run(
    config: BacktestConfig | dict | str | Path,
    *,
    run_dir: str | Path | None = None,
    chunk_rows: int = 65_536,
) -> RunResult:
    """Run one backtest and write its run directory. Never raises for a strategy error: the
    result has status "failed" and the traceback is in the manifest."""
    if isinstance(config, str | Path):
        config = BacktestConfig.from_yaml(config)
    elif isinstance(config, dict):
        config = BacktestConfig.model_validate(config)

    ds = Dataset(config.dataset)
    if config.dataset_id is not None and config.dataset_id != ds.id:
        raise ValueError(f"dataset id {ds.id} does not match the config's {config.dataset_id}")
    registry = ds.registry
    for s in config.universe:
        registry.by_symbol(s)
    kind = "bars" if config.execution.model == "candle" else "quotes"
    entries = [ds.file(kind, s) for s in config.universe]  # KeyError if the data is missing

    cls = resolve(config.strategy.name)
    strategy = cls(**config.strategy.params)
    strat_info = strategy_hash(cls, config.strategy.name, strategy.params)

    started = datetime.now(UTC)
    if run_dir is None:
        tag = hashlib.sha256(_canonical(config.to_json_dict())).hexdigest()[:8]
        run_dir = Path(config.output_dir) / f"{started:%Y%m%dT%H%M%SZ}-{config.strategy.name}-{tag}"
    run_dir = Path(run_dir)
    run_dir.mkdir(parents=True, exist_ok=False)

    x = config.execution
    engine = _engine.Backtest(
        registry.to_dicts(),
        x.model,
        slippage_kind=x.slippage.kind,
        slippage_rate=x.slippage.rate,
        participation=int(x.participation * 1_000_000),
        latency_ns=x.latency_ns,
        start_ns=config.start_ns or 0,
        equity_interval_ns=(config.equity_interval_s or 0) * 1_000_000_000,
    )
    ctx = Context(engine, registry, config.universe, config.seed)
    engine.set_handler(_Runner(strategy, ctx, registry))
    engine.open_account(**config.engine_account())
    # Manifest order: (instrument id, kind) — the EVT-2 tie-break.
    for entry in sorted(entries, key=lambda e: e["instrument_id"]):
        engine.add_feed(
            kind,
            entry["instrument_id"],
            _chunks(ds, entry, config.start_ns, config.end_ns, chunk_rows),
        )

    status, error = "completed", None
    t0 = time.perf_counter()
    try:
        engine.run()
        engine.finish()  # OP-3; ACC-10/11 checked in C++ (a ledger bug aborts)
        strategy.on_end(ctx)
    except Exception as exc:  # a strategy error: the run fails, artefacts so far are kept
        status = "failed"
        error = {
            "type": type(exc).__name__,
            "message": str(exc),
            "traceback": traceback.format_exc(),
        }
    wall = time.perf_counter() - t0

    orders = _orders_table(engine, ctx, registry)
    fills, borrow = _fills_table(engine, registry)
    equity = _equity_table(engine)
    artefacts = {}
    for name, table in (("orders", orders), ("fills", fills), ("equity", equity)):
        path = run_dir / f"{name}.parquet"
        pq.write_table(table, path)
        artefacts[path.name] = _sha256_file(path)

    account = engine.account()
    initial = config.engine_account()["cash"]
    manifest: dict[str, Any] = {
        "format": "finforge-run",
        "format_version": RUN_FORMAT_VERSION,
        "status": status,
        "partial": status != "completed",
        "error": error,
        "config": config.to_json_dict(),
        "dataset": {"id": ds.id, "name": ds.manifest["name"], "path": str(config.dataset)},
        "strategy": strat_info,
        "engine": _engine_info(),
        "execution_model": x.model,
        "seed": config.seed,
        "timings": {"started_utc": started.isoformat(), "wall_seconds": round(wall, 3)},
        "journal_hash": engine.journal_hash(),
        "counts": {
            **dict(engine.counts()),
            "fills": fills.num_rows,
            "sdk_rejected": len(ctx._local),
        },
        "reconciles": bool(engine.reconciles()),
        "summary": {
            "initial_equity": initial / MONEY_SCALE,
            "final_equity": account["equity"] / MONEY_SCALE,
            "return": account["equity"] / initial - 1.0,
            "realised_pnl": account["realised_pnl"] / MONEY_SCALE,
            "total_fees": account["total_fees"] / MONEY_SCALE,
            "borrow": borrow,
        },
        "artefacts": artefacts,
    }
    (run_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return RunResult(run_dir=run_dir, status=status, manifest=manifest, strategy=strategy)
