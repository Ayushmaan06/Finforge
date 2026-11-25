"""Parameter sweeps with an in-sample / out-of-sample split (PLAN Phase 6 task 7).

A sweep is a base backtest config, a grid of strategy parameters and a holdout date:

    base: examples/ma_cross.yaml        # path to a config, or an inline mapping
    grid: {fast: [10, 20, 30], slow: [120, 240]}
    holdout: 2024-01-20T00:00:00Z       # in-sample: start..holdout; out-of-sample: holdout..end
    processes: 4
    output_dir: runs/sweeps

Every grid point runs twice (in-sample and out-of-sample) as separate backtests on a process pool
(spawn start method, one engine per process; parallel sweeps never use threads, PLAN §4.3). The
out-of-sample run starts cold: indicators warm up again after the holdout date.

The results table has one row per grid point with `is_*` and `oos_*` metric columns and is written
as `results.parquet` and `results.json` in the sweep directory, next to every run directory.
"""

import itertools
import json
import multiprocessing as mp
import time
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

import pyarrow as pa
import pyarrow.parquet as pq
import yaml
from pydantic import BaseModel, ConfigDict, Field, field_validator

from finforge.config import BacktestConfig

METRICS = ("total_return", "cagr", "sharpe", "sortino", "max_drawdown", "volatility", "var")


class SweepConfig(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)

    base: str | dict[str, Any]
    grid: dict[str, list[Any]] = Field(min_length=1)
    holdout: datetime
    processes: int = Field(default=0, ge=0)  # 0 = os.cpu_count()
    output_dir: str = "runs/sweeps"
    benchmark: bool = False  # buy-and-hold per run is usually not needed in a sweep

    @field_validator("grid")
    @classmethod
    def _non_empty(cls, v: dict[str, list[Any]]) -> dict[str, list[Any]]:
        if any(len(vals) == 0 for vals in v.values()):
            raise ValueError("every grid parameter needs at least one value")
        return v

    @field_validator("holdout")
    @classmethod
    def _utc(cls, v: datetime) -> datetime:
        if v.tzinfo is None:
            raise ValueError("holdout must carry a timezone")
        return v

    @classmethod
    def from_yaml(cls, path: str | Path) -> "SweepConfig":
        # Paths (base, output_dir) are relative to the working directory, like in run configs.
        return cls.model_validate(yaml.safe_load(Path(path).read_text()))

    def base_config(self) -> BacktestConfig:
        if isinstance(self.base, str):
            return BacktestConfig.from_yaml(self.base)
        return BacktestConfig.model_validate(self.base)


def grid_points(grid: dict[str, list[Any]]) -> list[dict[str, Any]]:
    keys = list(grid)
    return [dict(zip(keys, values, strict=True)) for values in itertools.product(*grid.values())]


def _job(args: tuple[dict[str, Any], str]) -> dict[str, Any]:
    """Worker: one backtest in a fresh process. Returns its key metrics."""
    from finforge.backtest import run

    config, run_dir = args
    r = run(config, run_dir=run_dir)
    out: dict[str, Any] = {"status": r.status, "run_dir": run_dir}
    report_path = Path(run_dir) / "report.json"
    if r.status == "completed" and report_path.exists():
        rep = json.loads(report_path.read_text())
        out.update({k: rep["performance"][k] for k in METRICS})
        out["trades"] = rep["trades"]["count"]
        out["fees"] = r.manifest["summary"]["total_fees"]
    else:
        out["error"] = (r.manifest.get("error") or {}).get("message")
    return out


def run_sweep(sweep: SweepConfig | str | Path, out_dir: str | Path | None = None) -> pa.Table:
    if not isinstance(sweep, SweepConfig):
        sweep = SweepConfig.from_yaml(sweep)
    base = sweep.base_config()
    if (base.start and sweep.holdout <= base.start) or (base.end and sweep.holdout >= base.end):
        raise ValueError("holdout must lie strictly between the base config's start and end")
    points = grid_points(sweep.grid)
    if out_dir is None:
        out_dir = (
            Path(sweep.output_dir) / f"{datetime.now(UTC):%Y%m%dT%H%M%SZ}-{base.strategy.name}"
        )
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=False)

    jobs = []
    for i, params in enumerate(points):
        strategy = {"name": base.strategy.name, "params": {**base.strategy.params, **params}}
        for phase, start, end in (
            ("is", base.start, sweep.holdout),
            ("oos", sweep.holdout, base.end),
        ):
            cfg = base.model_copy(update={"benchmark": sweep.benchmark}).to_json_dict()
            cfg.update(
                {
                    "strategy": strategy,
                    "start": start.isoformat() if start else None,
                    "end": end.isoformat() if end else None,
                }
            )
            BacktestConfig.model_validate(cfg)  # fail before starting any process
            jobs.append((cfg, str(out_dir / f"{i:03d}-{phase}")))

    t0 = time.perf_counter()
    processes = sweep.processes or None
    with mp.get_context("spawn").Pool(processes) as pool:
        results = pool.map(_job, jobs, chunksize=1)
    wall = time.perf_counter() - t0

    rows = []
    for i, params in enumerate(points):
        is_r, oos_r = results[2 * i], results[2 * i + 1]
        row: dict[str, Any] = {"point": i, **{f"param_{k}": v for k, v in params.items()}}
        for prefix, r in (("is", is_r), ("oos", oos_r)):
            row[f"{prefix}_status"] = r["status"]
            for k in (*METRICS, "trades", "fees"):
                row[f"{prefix}_{k}"] = r.get(k)
        rows.append(row)
    table = pa.Table.from_pylist(rows)
    pq.write_table(table, out_dir / "results.parquet")
    (out_dir / "results.json").write_text(
        json.dumps(
            {
                "sweep": sweep.model_dump(mode="json"),
                "points": len(points),
                "runs": len(jobs),
                "processes": processes,
                "wall_seconds": round(wall, 3),
                "results": rows,
            },
            indent=2,
            default=str,
        )
        + "\n"
    )
    return table


def format_results(table: pa.Table) -> str:
    rows = table.to_pylist()
    params = [c for c in table.column_names if c.startswith("param_")]
    cols = [
        *params,
        "is_total_return",
        "is_sharpe",
        "is_max_drawdown",
        "oos_total_return",
        "oos_sharpe",
        "oos_max_drawdown",
    ]
    head = [c.removeprefix("param_") for c in cols]

    def fmt(c: str, v: Any) -> str:
        if v is None:
            return "-"
        if c.startswith("param_"):
            return str(v)
        if "sharpe" in c:
            return f"{v:.2f}"
        return f"{v:.2%}" if "drawdown" in c else f"{v:+.2%}"

    lines = [head] + [[fmt(c, r[c]) for c in cols] for r in rows]
    widths = [max(len(line[i]) for line in lines) for i in range(len(cols))]
    return "\n".join(
        "  ".join(x.rjust(w) for x, w in zip(line, widths, strict=True)) for line in lines
    )
