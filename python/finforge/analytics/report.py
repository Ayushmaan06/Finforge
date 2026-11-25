"""`report.json` for a run directory (PLAN Phase 6 task 5).

Built only from the run's artefacts (manifest, equity, fills, exposure), so it can be regenerated
at any time with `finforge report RUN_DIR`. Also writes `trades.parquet` (FIFO round trips).
"""

import hashlib
import json
import math
from decimal import Decimal
from pathlib import Path
from typing import Any

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

from finforge.analytics.exposure import exposure, exposure_summary, pnl_by_instrument
from finforge.analytics.metrics import DEFAULT_CONVENTIONS, Conventions, performance
from finforge.analytics.trades import fifo_trades, trade_summary
from finforge.data.fixedpoint import Grid
from finforge.sdk import from_grid

REPORT_FORMAT_VERSION = 1
MONEY = 100_000_000


def _clean(x: Any) -> Any:
    """JSON-safe: numpy scalars to Python, NaN/inf to None."""
    if isinstance(x, dict):
        return {k: _clean(v) for k, v in x.items()}
    if isinstance(x, list | tuple):
        return [_clean(v) for v in x]
    if isinstance(x, np.generic):
        x = x.item()
    if isinstance(x, float) and not math.isfinite(x):
        return None
    return x


def _units(manifest: dict[str, Any]) -> dict[str, tuple[int, int, float]]:
    return {
        i["symbol"]: (
            i["money_per_tick_lot"],
            i["contract_multiplier"],
            from_grid(1, Grid.parse(i["lot_size"])),
        )
        for i in manifest["instruments"]
    }


def equity_performance(run_dir: Path, conv: Conventions) -> dict[str, Any]:
    m = json.loads((run_dir / "manifest.json").read_text())
    eq = pq.read_table(run_dir / "equity.parquet")
    return performance(
        eq.column("ts").to_numpy(),
        eq.column("equity_real").to_numpy(),
        m["summary"]["initial_equity"],
        conv,
    )


def build_report(
    run_dir: str | Path,
    benchmark_dir: str | Path | None = None,
    conv: Conventions = DEFAULT_CONVENTIONS,
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    run_dir = Path(run_dir)
    m = json.loads((run_dir / "manifest.json").read_text())
    units = _units(m)
    eq = pq.read_table(run_dir / "equity.parquet")
    fills = pq.read_table(run_dir / "fills.parquet").to_pylist()
    ex = pq.read_table(run_dir / "exposure.parquet")

    perf = equity_performance(run_dir, conv)
    trades, open_lots = fifo_trades(fills, units)

    symbols = [i["symbol"] for i in m["instruments"]]
    n = eq.num_rows
    k = len(symbols)
    pos = ex.column("position").to_numpy().reshape(n, k) if n else np.zeros((0, k), np.int64)
    mark = ex.column("mark").to_numpy().reshape(n, k) if n else np.zeros((0, k), np.int64)
    unit_value = np.array([units[s][0] * units[s][1] / MONEY for s in symbols])
    equity_real = eq.column("equity_real").to_numpy()
    series = exposure(equity_real, pos, mark, unit_value)
    final_pos = {s: int(pos[-1, j]) if n else 0 for j, s in enumerate(symbols)}
    final_mark = {s: int(mark[-1, j]) if n else 0 for j, s in enumerate(symbols)}
    by_instrument = pnl_by_instrument(fills, final_pos, final_mark, units)

    # Identity: Σ instrument P&L - borrow == final equity - initial capital, exactly in Money.
    initial_money = int(Decimal(str(m["config"]["initial_capital"])) * MONEY)  # exact
    final_money = int(eq.column("equity")[-1].as_py()) if n else initial_money
    borrow = m["summary"]["borrow"]["total"]
    identity = (
        sum(v["pnl_money"] for v in by_instrument.values()) - borrow == final_money - initial_money
    )
    for v in by_instrument.values():
        v.pop("pnl_money")

    report: dict[str, Any] = {
        "format": "finforge-report",
        "format_version": REPORT_FORMAT_VERSION,
        "run": {
            "strategy": m["strategy"]["name"],
            "params": m["strategy"]["params"],
            "strategy_hash": m["strategy"]["hash"],
            "dataset_id": m["dataset"]["id"],
            "execution_model": m["execution_model"],
            "journal_hash": m["journal_hash"],
            "status": m["status"],
        },
        "conventions": conv.to_dict(),
        "performance": perf,
        "trades": trade_summary(trades, open_lots),
        "exposure": exposure_summary(series, equity_real),
        "instruments": by_instrument,
        "pnl_identity_holds": bool(identity),
        "benchmark": None,
    }
    if benchmark_dir is not None:
        bdir = Path(benchmark_dir)
        bench = equity_performance(bdir, conv)
        report["benchmark"] = {
            "strategy": "buy_and_hold",
            "run_dir": str(bdir.relative_to(run_dir) if bdir.is_relative_to(run_dir) else bdir),
            "performance": bench,
            "excess_total_return": perf["total_return"] - bench["total_return"],
        }
    return _clean(report), trades


TRADES_SCHEMA = pa.schema(
    [
        ("symbol", pa.string()),
        ("direction", pa.string()),
        ("entry_ts", pa.int64()),
        ("exit_ts", pa.int64()),
        ("duration_ns", pa.int64()),
        ("qty", pa.float64()),
        ("entry_price", pa.float64()),
        ("exit_price", pa.float64()),
        ("gross_pnl", pa.float64()),
        ("fees", pa.float64()),
        ("net_pnl", pa.float64()),
        ("price_cost", pa.float64()),
        ("execution_cost", pa.float64()),
    ]
)


def write_report(
    run_dir: str | Path,
    benchmark_dir: str | Path | None = None,
    conv: Conventions = DEFAULT_CONVENTIONS,
) -> dict[str, Any]:
    run_dir = Path(run_dir)
    report, trades = build_report(run_dir, benchmark_dir, conv)
    table = pa.Table.from_pylist(trades, schema=TRADES_SCHEMA)
    pq.write_table(table, run_dir / "trades.parquet")
    report["artefacts"] = {
        "trades.parquet": hashlib.sha256((run_dir / "trades.parquet").read_bytes()).hexdigest()
    }
    (run_dir / "report.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    return report
