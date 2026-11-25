"""Shared fixtures: small crafted datasets built through the real ingest path."""

from collections.abc import Callable, Sequence
from pathlib import Path

import pytest

from finforge.data import ingest

MINUTE_MS = 60_000
T0_MS = 1_709_251_200_000  # 2024-03-01T00:00:00Z
T0_NS = T0_MS * 1_000_000
MINUTE_NS = MINUTE_MS * 1_000_000

INSTRUMENTS = [
    {
        "symbol": "AAA",
        "asset_class": "spot",
        "quote_currency": "USDT",
        "tick_size": "0.01",
        "lot_size": "0.001",
        "min_qty": 1,
        "contract_multiplier": 1,
    },
    {
        "symbol": "BBB",
        "asset_class": "spot",
        "quote_currency": "USDT",
        "tick_size": "0.01",
        "lot_size": "0.001",
        "min_qty": 1,
        "contract_multiplier": 1,
    },
]

Row = tuple[float, float, float, float, float]  # open, high, low, close, volume


def bars_from_closes(closes: Sequence[float]) -> list[Row]:
    """Bars that open and close at the given price (so a next-bar fill price is the close)."""
    return [(c, c, c, c, 100.0) for c in closes]


@pytest.fixture
def make_dataset(tmp_path: Path) -> Callable[..., Path]:
    """make_dataset({"AAA": rows, ...}) -> dataset directory of 1-minute bars from T0."""

    def make(series: dict[str, Sequence[Row]], name: str = "crafted") -> Path:
        root = tmp_path / f"src-{name}"
        root.mkdir()
        sources = []
        for sym, rows in series.items():
            lines = ["time,o,h,l,c,v"]
            for i, (o, h, lo, c, v) in enumerate(rows):
                lines.append(f"{T0_MS + i * MINUTE_MS},{o:.2f},{h:.2f},{lo:.2f},{c:.2f},{v:.3f}")
            (root / f"{sym}.csv").write_text("\n".join(lines) + "\n")
            sources.append(
                {
                    "format": "generic",
                    "kind": "bars",
                    "symbol": sym,
                    "interval": "1m",
                    "paths": [f"{sym}.csv"],
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
        out = tmp_path / f"ds-{name}"
        ingest({"name": name, "instruments": INSTRUMENTS, "sources": sources}, out, root=root)
        return out

    return make


def config(dataset: Path, strategy: str, params: dict, tmp_path: Path, **extra) -> dict:
    base = {
        "dataset": str(dataset),
        "universe": ["AAA"],
        "initial_capital": "100000",
        "strategy": {"name": strategy, "params": params},
        "output_dir": str(tmp_path / "runs"),
    }
    base.update(extra)
    return base
