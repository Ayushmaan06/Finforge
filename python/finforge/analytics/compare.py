"""Compare N runs (PLAN Phase 6 task 6): a metrics table plus equity and drawdown series aligned on
UTC daily closes.

    from finforge.analytics.compare import compare
    c = compare(["runs/a", "runs/b"])
    print(c.format_table())
    c.series            # pyarrow Table: ts, <label>_equity (growth of 1), <label>_drawdown
"""

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

from finforge.analytics.metrics import DEFAULT_CONVENTIONS, Conventions, daily_closes, performance

COLUMNS = [
    ("total_return", "return", "{:+.2%}"),
    ("cagr", "cagr", "{:+.2%}"),
    ("volatility", "vol", "{:.2%}"),
    ("sharpe", "sharpe", "{:.2f}"),
    ("sortino", "sortino", "{:.2f}"),
    ("max_drawdown", "max dd", "{:.2%}"),
    ("var", "VaR95", "{:.2%}"),
    ("trades", "trades", "{}"),
    ("win_rate", "win", "{:.0%}"),
    ("profit_factor", "PF", "{:.2f}"),
    ("fees", "fees", "{:.2f}"),
]


@dataclass
class Comparison:
    rows: list[dict[str, Any]]
    series: pa.Table

    def format_table(self) -> str:
        head = ["run"] + [h for _, h, _ in COLUMNS]
        lines = [head]
        for r in self.rows:
            cells = [r["label"]]
            for key, _, fmt in COLUMNS:
                v = r.get(key)
                cells.append("-" if v is None else fmt.format(v))
            lines.append(cells)
        widths = [max(len(row[i]) for row in lines) for i in range(len(head))]
        return "\n".join(
            "  ".join(
                c.rjust(w) if i else c.ljust(w)
                for i, (c, w) in enumerate(zip(row, widths, strict=True))
            )
            for row in lines
        )


def _label(run_dir: Path, manifest: dict[str, Any], used: set[str]) -> str:
    base = f"{manifest['strategy']['name']}:{run_dir.name}"
    label, k = base, 2
    while label in used:
        label, k = f"{base}#{k}", k + 1
    used.add(label)
    return label


SPLIT_METRICS = ("total_return", "sharpe", "sortino", "max_drawdown", "volatility")


def split_performance(
    ts: np.ndarray, equity: np.ndarray, initial: float, split_ns: int, conv: Conventions
) -> dict[str, Any]:
    """Metrics before and after `split_ns`: in-sample from the initial capital, out-of-sample
    from the equity at the split (the last point before it)."""
    before = ts < split_ns
    out: dict[str, Any] = {}
    is_perf = performance(ts[before], equity[before], initial, conv)
    start = float(equity[before][-1]) if before.any() else initial
    oos_perf = performance(ts[~before], equity[~before], start, conv)
    for k in SPLIT_METRICS:
        out[f"is_{k}"] = is_perf[k] if before.any() else None
        out[f"oos_{k}"] = oos_perf[k] if (~before).any() else None
    return out


def compare(
    run_dirs: list[str | Path], conv: Conventions = DEFAULT_CONVENTIONS, split_ns: int | None = None
) -> Comparison:
    rows: list[dict[str, Any]] = []
    curves: list[tuple[str, np.ndarray, np.ndarray, float]] = []
    used: set[str] = set()
    for d in map(Path, run_dirs):
        m = json.loads((d / "manifest.json").read_text())
        eq = pq.read_table(d / "equity.parquet")
        initial = m["summary"]["initial_equity"]
        ts, equity = eq.column("ts").to_numpy(), eq.column("equity_real").to_numpy()
        perf = performance(ts, equity, initial, conv)
        report = json.loads((d / "report.json").read_text()) if (d / "report.json").exists() else {}
        t = report.get("trades", {})
        label = _label(d, m, used)
        rows.append(
            {
                "label": label,
                "run_dir": str(d),
                "status": m["status"],
                "strategy": m["strategy"]["name"],
                "params": m["strategy"]["params"],
                **{
                    k: perf[k]
                    for k in (
                        "total_return",
                        "cagr",
                        "volatility",
                        "sharpe",
                        "sortino",
                        "max_drawdown",
                        "var",
                        "cvar",
                    )
                },
                "trades": t.get("count"),
                "win_rate": t.get("win_rate"),
                "profit_factor": t.get("profit_factor"),
                "fees": m["summary"]["total_fees"],
            }
        )
        if split_ns is not None:
            rows[-1].update(split_performance(ts, equity, initial, split_ns, conv))
        days, closes = daily_closes(ts, equity)
        curves.append((label, days, closes, initial))

    all_days = (
        np.unique(np.concatenate([c[1] for c in curves])) if curves else np.empty(0, np.int64)
    )
    cols: dict[str, Any] = {"ts": pa.array(all_days, pa.int64())}
    for label, days, closes, initial in curves:
        # Growth of 1, forward-filled; 1.0 before the run's first day.
        idx = np.searchsorted(days, all_days, side="right") - 1
        growth = np.where(idx >= 0, closes[np.maximum(idx, 0)] / initial, 1.0)
        peak = np.maximum.accumulate(np.maximum(growth, 1.0))
        cols[f"{label}_equity"] = pa.array(growth, pa.float64())
        cols[f"{label}_drawdown"] = pa.array(growth / peak - 1.0, pa.float64())
    return Comparison(rows=rows, series=pa.table(cols))
