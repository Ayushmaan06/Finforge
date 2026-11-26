"""FIFO trades, exposure, report.json, benchmark, compare and the parallel sweep (PLAN Phase 6)."""

import json
from pathlib import Path

import numpy as np
import pyarrow.parquet as pq
import pytest
from conftest import MINUTE_NS, T0_NS, bars_from_closes, config

from finforge.analytics.compare import compare, split_performance
from finforge.analytics.exposure import exposure, pnl_by_instrument
from finforge.analytics.metrics import DEFAULT_CONVENTIONS
from finforge.analytics.sweep import SweepConfig, format_results, grid_points, run_sweep
from finforge.analytics.trades import fifo_trades, trade_summary
from finforge.backtest import run
from finforge.cli import main as cli

U = 100_000_000
UNITS = {"X": (U, 1, 1.0)}  # tick 1, lot 1: one lot at one tick is one unit of currency


def fill(ts, side, price, qty, fee, ref=None):
    return {
        "ts": ts,
        "symbol": "X",
        "side": side,
        "price": price,
        "qty": qty,
        "fee": fee,
        "price_real": float(price),
        "qty_real": float(qty),
        "reference_real": float("nan") if ref is None else ref,
    }


def test_fifo_round_trips_by_hand() -> None:
    fills = [
        fill(1, "buy", 100, 10, 100_000_000, ref=99.0),  # fee 1, 1 unit worse than reference
        fill(2, "buy", 110, 5, 55_000_000),  # fee 0.55
        fill(3, "sell", 120, 12, 144_000_000, ref=121.0),  # fee 1.44, 1 unit worse
        fill(4, "sell", 90, 8, 72_000_000),  # fee 0.72: closes 3, opens short 5
        fill(5, "buy", 80, 5, 40_000_000),  # fee 0.40: covers the short
    ]
    trades, open_lots = fifo_trades(fills, UNITS)
    got = [
        (
            t["direction"],
            t["entry_ts"],
            t["exit_ts"],
            t["qty"],
            t["gross_pnl"],
            round(t["fees"], 10),
            round(t["net_pnl"], 10),
            t["price_cost"],
        )
        for t in trades
    ]
    assert got == [
        # 10 @100 → 120: +200; fees 1 x 10/10 + 1.44 x 10/12 = 2.2; price cost 1x10 + 1x10
        ("long", 1, 3, 10.0, 200.0, 2.2, 197.8, 20.0),
        # 2 of 5 @110 → 120: +20; fees 0.55 x 2/5 + 1.44 x 2/12 = 0.46; cost 0 + 1x2
        ("long", 2, 3, 2.0, 20.0, 0.46, 19.54, 2.0),
        # 3 of 5 @110 → 90: -60; fees 0.55 x 3/5 + 0.72 x 3/8 = 0.6
        ("long", 2, 4, 3.0, -60.0, 0.6, -60.6, 0.0),
        # short 5 @90 → 80: +50; fees 0.72 x 5/8 + 0.4 = 0.85
        ("short", 4, 5, 5.0, 50.0, 0.85, 49.15, 0.0),
    ]
    assert open_lots == []
    s = trade_summary(trades, open_lots)
    assert (s["count"], s["wins"], s["losses"], s["win_rate"]) == (4, 3, 1, 0.75)
    assert s["profit_factor"] == pytest.approx((197.8 + 19.54 + 49.15) / 60.6, abs=1e-9)
    assert s["fees"] == pytest.approx(4.11, abs=1e-9)  # every fee allocated exactly once
    assert s["execution_cost"] == pytest.approx(4.11 + 22.0, abs=1e-9)
    assert s["holding_period"]["median_s"] == pytest.approx(1.5e-9)


def test_profit_factor_infinite_and_open_lots() -> None:
    trades, open_lots = fifo_trades(
        [fill(1, "buy", 10, 2, 0), fill(2, "sell", 12, 1, 0), fill(3, "sell", 9, 4, 0)], UNITS
    )
    s = trade_summary(trades[:1], open_lots)
    assert s["profit_factor"] is None and s["profit_factor_infinite"] is True
    assert open_lots == [
        {"symbol": "X", "direction": "short", "entry_ts": 3, "qty": 3.0, "entry_price": 9.0}
    ]
    assert trade_summary([], [])["win_rate"] is None


def test_exposure_and_instrument_pnl_by_hand() -> None:
    position = np.array([[0, 0], [2, -1], [2, 0]])
    mark = np.array([[10, 50], [11, 40], [12, 40]])
    s = exposure(np.array([100.0, 100.0, 104.0]), position, mark, np.array([1.0, 1.0]))
    assert s["gross"].tolist() == [0.0, 62.0, 24.0]  # |2x11| + |-1x40|
    assert s["net"].tolist() == [0.0, -18.0, 24.0]
    assert s["max_share"].tolist() == pytest.approx([0.0, 40 / 62, 1.0])
    assert s["hhi"].tolist() == pytest.approx([0.0, (22 / 62) ** 2 + (40 / 62) ** 2, 1.0])
    assert s["utilisation"].tolist() == pytest.approx([0.0, 0.62, 24 / 104])
    pnl = pnl_by_instrument([fill(1, "buy", 10, 2, 5)], {"X": 2}, {"X": 12}, UNITS)
    assert pnl["X"]["pnl"] == (24 * U - 20 * U - 5) / U  # value - cost - fee


@pytest.fixture
def two_symbols(make_dataset) -> Path:
    rng = np.random.default_rng(11)
    a = 100 + np.cumsum(rng.integers(-3, 4, 240)) * 0.25
    b = 50 + np.cumsum(rng.integers(-2, 3, 240)) * 0.1
    return make_dataset({"AAA": bars_from_closes(a.round(2)), "BBB": bars_from_closes(b.round(2))})


def test_report_for_every_completed_run(two_symbols: Path, tmp_path: Path) -> None:
    cfg = config(
        two_symbols,
        "momentum",
        {"lookback": 12, "rebalance": 4, "qty": "2"},
        tmp_path,
        universe=["AAA", "BBB"],
        fees={"taker_bps": 10},
        execution={"slippage": {"kind": "fixed_bps", "bps": 5}},
    )
    r = run(cfg, run_dir=tmp_path / "r")
    rep = json.loads((r.run_dir / "report.json").read_text())
    p = rep["performance"]
    assert p["total_return"] == pytest.approx(r.manifest["summary"]["return"], abs=1e-12)
    assert rep["pnl_identity_holds"] is True
    assert rep["conventions"]["periods_per_year"] == 365
    trades = pq.read_table(r.run_dir / "trades.parquet")
    assert trades.num_rows == rep["trades"]["count"] > 0
    # Slippage shows up as execution cost on top of fees (every fill is a slipped market order).
    assert rep["trades"]["price_cost"] > 0
    assert rep["exposure"]["points"] == 240 and 0 < rep["exposure"]["time_in_market"] <= 1
    assert set(rep["instruments"]) == {"AAA", "BBB"}
    # Benchmark: buy-and-hold of both, same capital/fees/slippage/model, one order each.
    bench = rep["benchmark"]
    assert bench["run_dir"] == "benchmark"
    bm = json.loads((r.run_dir / "benchmark" / "manifest.json").read_text())
    assert bm["strategy"]["name"] == "buy_and_hold" and bm["counts"]["fills"] == 2
    assert bm["config"]["fees"] == r.manifest["config"]["fees"]
    assert bench["excess_total_return"] == pytest.approx(
        p["total_return"] - bench["performance"]["total_return"]
    )
    # The report can be regenerated from the artefacts alone.
    (r.run_dir / "report.json").unlink()
    assert cli(["report", str(r.run_dir)]) == 0
    assert json.loads((r.run_dir / "report.json").read_text())["performance"] == rep["performance"]


def test_failed_run_has_no_report(make_dataset, tmp_path: Path) -> None:
    strat = tmp_path / "boom.py"
    strat.write_text(
        "from finforge.sdk import Strategy\n\nclass Boom(Strategy):\n"
        "    def on_bar(self, ctx, bar):\n        raise ValueError('x')\n"
    )
    ds = make_dataset({"AAA": bars_from_closes([10, 11, 12])})
    r = run(config(ds, f"{strat}:Boom", {}, tmp_path), run_dir=tmp_path / "f")
    assert r.status == "failed"
    assert not (r.run_dir / "report.json").exists() and not (r.run_dir / "benchmark").exists()


def test_split_point_at_the_boundary_is_in_sample() -> None:
    # Hourly closes over two days; the bar closing exactly at the split jumps 100 -> 110.
    hour, day = 3_600 * 10**9, 86_400 * 10**9
    ts = np.arange(1, 49, dtype=np.int64) * hour
    equity = np.where(ts >= day, 110.0, 100.0)
    s = split_performance(ts, equity, 100.0, day, DEFAULT_CONVENTIONS)
    assert s["is_total_return"] == pytest.approx(0.10)
    assert s["oos_total_return"] == 0.0


def test_compare_table_tells_infinite_from_undefined() -> None:
    import pyarrow as pa

    from finforge.analytics.compare import Comparison

    rows = [
        {"label": "no-losses", "profit_factor": None, "profit_factor_infinite": True},
        {"label": "no-trades", "profit_factor": None, "profit_factor_infinite": False},
    ]
    table = Comparison(rows, pa.table({})).format_table().splitlines()
    assert table[0].split()[-2] == "PF"  # columns: ..., PF, fees
    assert [line.split()[-2] for line in table[1:]] == ["inf", "-"]


def test_compare_aligns_runs(two_symbols: Path, tmp_path: Path) -> None:
    a = run(
        config(two_symbols, "momentum", {"lookback": 12, "rebalance": 4}, tmp_path),
        run_dir=tmp_path / "a",
    )
    b = run(
        config(
            two_symbols, "ma_cross", {"fast": 3, "slow": 9}, tmp_path, start="2024-03-01T01:00:00Z"
        ),
        run_dir=tmp_path / "b",
    )
    c = compare([a.run_dir, b.run_dir])
    assert [row["strategy"] for row in c.rows] == ["momentum", "ma_cross"]
    assert c.rows[0]["total_return"] == pytest.approx(a.manifest["summary"]["return"])
    names = c.series.column_names
    assert names[0] == "ts" and len(names) == 5 and names[1].endswith("_equity")
    eq = c.series.column(names[1]).to_numpy()
    assert eq[-1] == pytest.approx(1 + a.manifest["summary"]["return"])
    assert (c.series.column(names[2]).to_numpy() <= 0).all()  # drawdowns
    assert "momentum" in c.format_table()
    assert cli(["compare", str(a.run_dir), str(b.run_dir), "--out", str(tmp_path / "cmp")]) == 0
    assert (tmp_path / "cmp" / "series.parquet").exists()


def test_ten_config_sweep_runs_in_parallel_with_is_and_oos(make_dataset, tmp_path: Path) -> None:
    rng = np.random.default_rng(3)
    closes = (100 + np.cumsum(rng.integers(-3, 4, 600)) * 0.5).round(2)
    ds = make_dataset({"AAA": bars_from_closes(closes)})
    base = config(
        ds,
        "ma_cross",
        {"qty": "1"},
        tmp_path,
        fees={"taker_bps": 5},
        start="2024-03-01T00:00:00Z",
        end="2024-03-01T10:00:00Z",
    )
    sweep = SweepConfig.model_validate(
        {
            "base": base,
            "grid": {"fast": [2, 3, 5, 8, 13], "slow": [21, 34]},
            "holdout": "2024-03-01T06:00:00Z",
            "processes": 4,
        }
    )
    assert len(grid_points(sweep.grid)) == 10
    table = run_sweep(sweep, tmp_path / "sweep")
    rows = table.to_pylist()
    assert len(rows) == 10
    for col in (
        "is_total_return",
        "is_sharpe",
        "is_max_drawdown",
        "oos_total_return",
        "oos_sharpe",
        "oos_max_drawdown",
    ):
        assert col in table.column_names
    assert all(r["is_status"] == r["oos_status"] == "completed" for r in rows)
    assert {(r["param_fast"], r["param_slow"]) for r in rows} == {
        (f, s) for f in (2, 3, 5, 8, 13) for s in (21, 34)
    }
    # In-sample and out-of-sample really cover different windows.
    first_is = json.loads((tmp_path / "sweep" / "000-is" / "manifest.json").read_text())
    first_oos = json.loads((tmp_path / "sweep" / "000-oos" / "manifest.json").read_text())
    assert first_is["config"]["end"] == first_oos["config"]["start"]
    is_eq = pq.read_table(tmp_path / "sweep" / "000-is" / "equity.parquet").column("ts").to_numpy()
    oos_eq = (
        pq.read_table(tmp_path / "sweep" / "000-oos" / "equity.parquet").column("ts").to_numpy()
    )
    assert is_eq.max() <= T0_NS + 6 * 60 * MINUTE_NS <= oos_eq.min() - MINUTE_NS
    summary = json.loads((tmp_path / "sweep" / "results.json").read_text())
    assert summary["runs"] == 20 and summary["processes"] == 4
    assert "oos_sharpe" in format_results(table)
