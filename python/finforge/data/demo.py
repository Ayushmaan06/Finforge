"""A synthetic demo dataset: one year of 1-minute bars (no download needed).

Used by the dashboard's end-to-end test and for trying the platform without Binance data. The
prices follow a seeded random walk with drift and volatility regimes, so trend-following and
mean-reversion strategies both have something to find; it is not meant to look like any real
market. Built through the normal ingest path (generic Parquet source → validated dataset).
"""

from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

from finforge.data.ingest import ingest

T0_MS = 1_704_067_200_000  # 2024-01-01T00:00:00Z
MINUTE_MS = 60_000


def _bars(n: int, start_price: float, seed: int) -> pa.Table:
    rng = np.random.default_rng(seed)
    # Volatility regimes of ~10 days, small drift; log returns per minute.
    regime = np.repeat(rng.choice([0.0004, 0.0008, 0.0015], size=n // 14_400 + 1), 14_400)[:n]
    drift = np.repeat(rng.normal(0, 0.00002, size=n // 43_200 + 1), 43_200)[:n]
    r = drift + regime * rng.standard_normal(n)
    close = start_price * np.exp(np.cumsum(r))
    open_ = np.r_[start_price, close[:-1]]
    wiggle = np.abs(rng.standard_normal(n)) * regime * close
    high = np.maximum(open_, close) + wiggle
    low = np.maximum(np.minimum(open_, close) - wiggle, 0.01)
    volume = rng.gamma(2.0, 5.0, n)
    return pa.table(
        {
            "time": np.int64(T0_MS) + np.arange(n, dtype=np.int64) * MINUTE_MS,
            "o": np.round(open_, 2),
            "h": np.round(high, 2),
            "l": np.round(low, 2),
            "c": np.round(close, 2),
            "v": np.round(volume, 3),
        }
    )


def make_demo_dataset(
    out: str | Path,
    days: int = 365,
    symbols: tuple[str, ...] = ("DEMO",),
    seed: int = 7,
    force: bool = False,
) -> Path:
    """Write the dataset to `out` (a finforge.data dataset directory) and return its path."""
    out = Path(out)
    src = out.with_name(out.name + "-src")
    src.mkdir(parents=True, exist_ok=True)
    n = days * 1440
    sources = []
    for i, sym in enumerate(symbols):
        path = src / f"{sym}.parquet"
        if not path.exists() or force:
            pq.write_table(_bars(n, 100.0 * (i + 1), seed + i), path)
        sources.append(
            {
                "format": "generic",
                "kind": "bars",
                "symbol": sym,
                "interval": "1m",
                "paths": [path.name],
                "ts_unit": "ms",
                "columns": {
                    "ts_open": "time",
                    "open": "o",
                    "high": "h",
                    "low": "l",
                    "close": "c",
                    "volume": "v",
                },
            }
        )
    instruments = [
        {
            "symbol": s,
            "asset_class": "spot",
            "quote_currency": "USD",
            "tick_size": "0.01",
            "lot_size": "0.001",
            "min_qty": 1,
            "contract_multiplier": 1,
        }
        for s in symbols
    ]
    spec = {
        "name": f"demo-{days}d-1m",
        "instruments": instruments,
        "sources": sources,
        "notes": ["Synthetic random-walk bars for demos and tests (finforge.data.demo)."],
    }
    ingest(spec, out, root=src, force=force or out.exists())
    return out
