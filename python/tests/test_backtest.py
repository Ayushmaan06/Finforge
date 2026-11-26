"""End-to-end runs: artefacts, determinism, look-ahead, failure handling, config validation, CLI
(PLAN Phase 5 exit criteria 2-4, tasks 3-5, 7)."""

import json
import textwrap
from decimal import Decimal
from pathlib import Path

import numpy as np
import pytest
from conftest import MINUTE_NS, T0_NS, bars_from_closes, config
from pydantic import ValidationError

from finforge.backtest import run
from finforge.cli import main as cli
from finforge.config import BacktestConfig
from finforge.data import ingest
from finforge.sdk import Grid, to_grid

FIXTURES = Path(__file__).resolve().parents[2] / "datasets" / "fixtures"


def write_strategy(tmp_path: Path, body: str, name: str = "Custom") -> str:
    path = tmp_path / f"strat_{name.lower()}.py"
    path.write_text("from finforge.sdk import Strategy\n\n" + textwrap.dedent(body))
    return f"{path}:{name}"


@pytest.fixture
def two_symbols(make_dataset) -> Path:
    rng = np.random.default_rng(5)
    a = 100 + np.cumsum(rng.integers(-3, 4, 200)) * 0.25
    b = 50 + np.cumsum(rng.integers(-2, 3, 200)) * 0.1
    return make_dataset({"AAA": bars_from_closes(a.round(2)), "BBB": bars_from_closes(b.round(2))})


# --- Artefacts and determinism (exit criterion 2) ------------------------------------------------


def test_artefacts_and_manifest(two_symbols: Path, tmp_path: Path) -> None:
    cfg = config(
        two_symbols,
        "momentum",
        {"lookback": 10, "rebalance": 3, "qty": "1"},
        tmp_path,
        universe=["AAA", "BBB"],
        fees={"taker_bps": 10},
    )
    r = run(cfg, run_dir=tmp_path / "r1")
    assert r.status == "completed"
    assert sorted(p.name for p in r.run_dir.iterdir()) == [
        "benchmark",
        "equity.parquet",
        "exposure.parquet",
        "fills.parquet",
        "manifest.json",
        "orders.parquet",
        "report.json",
        "trades.parquet",
    ]
    m = json.loads((r.run_dir / "manifest.json").read_text())
    assert m == r.manifest
    assert m["dataset"]["id"] and m["strategy"]["hash"] and len(m["journal_hash"]) == 64
    assert m["engine"]["build_type"] == "Release" and m["engine"]["git_sha"]
    assert m["config"]["strategy"]["name"] == "momentum" and m["seed"] == 0
    assert m["reconciles"] and not m["partial"] and m["error"] is None
    fills = r.read("fills")
    assert fills.num_rows == m["counts"]["fills"] > 0
    assert set(fills.column("symbol").to_pylist()) == {"AAA", "BBB"}
    equity = r.read("equity")
    assert equity.num_rows == 200  # one point per bar timestamp (two instruments share it)
    assert equity.column("equity_real")[-1].as_py() == pytest.approx(m["summary"]["final_equity"])
    # Fees: every fill pays ceil(notional x 10 bps), so equity accounting holds.
    assert m["summary"]["total_fees"] == pytest.approx(sum(fills.column("fee_real").to_pylist()))


def test_same_config_twice_is_byte_identical(two_symbols: Path, tmp_path: Path) -> None:
    cfg = config(
        two_symbols,
        "mean_reversion",
        {"lookback": 15, "entry_z": 1.0, "exit_z": 0.2, "qty": "1", "allow_short": True},
        tmp_path,
        universe=["AAA", "BBB"],
        fees={"taker_bps": 7.5},
        execution={"slippage": {"kind": "half_spread", "bps": 3}},
        risk={"allow_short": True, "max_leverage": 2},
    )
    a = run(cfg, run_dir=tmp_path / "a")
    b = run(cfg, run_dir=tmp_path / "b")
    assert a.journal_hash == b.journal_hash
    assert a.manifest["artefacts"] == b.manifest["artefacts"]
    for name in ("orders", "fills", "equity"):
        assert (a.run_dir / f"{name}.parquet").read_bytes() == (
            b.run_dir / f"{name}.parquet"
        ).read_bytes()
    # A different parameter changes the strategy hash (D6) and the run.
    cfg["strategy"]["params"]["lookback"] = 16
    c = run(cfg, run_dir=tmp_path / "c")
    assert c.manifest["strategy"]["hash"] != a.manifest["strategy"]["hash"]


# --- Look-ahead (exit criterion 3) ---------------------------------------------------------------

LOOKAHEAD = """
class Checker(Strategy):
    def setup(self):
        self.calls = 0
        self.seen = {}

    def on_bar(self, ctx, bar):
        self.calls += 1
        self.seen[bar.symbol] = self.seen.get(bar.symbol, 0) + 1
        assert bar.ts <= ctx.now
        for sym in ctx.symbols:
            ts = ctx.history(sym, "ts")                       # everything ever delivered
            assert len(ts) == self.seen.get(sym, 0)
            assert (ts <= ctx.now).all(), "history beyond now"
            assert len(ctx.history(sym, "close", 10**9)) == len(ts)
        close = ctx.history(bar.symbol, "close", 3)
        assert close[-1] == bar.close
        try:
            close[0] = 0.0
        except ValueError:
            pass
        else:
            raise AssertionError("history must be read-only")
        if self.calls % 7 == 0:
            ctx.submit_market(bar.symbol, "buy", "0.001")

    def on_quote(self, ctx, quote):
        self.calls += 1
        ts = ctx.history(quote.symbol, "ts", kind="quotes")
        assert (ts <= ctx.now).all() and ts[-1] == quote.ts == ctx.now
"""


def test_history_never_returns_data_later_than_now(two_symbols: Path, tmp_path: Path) -> None:
    name = write_strategy(tmp_path, LOOKAHEAD, "Checker")
    r = run(config(two_symbols, name, {}, tmp_path, universe=["AAA", "BBB"]))
    assert r.status == "completed", r.manifest["error"]
    assert r.strategy.calls == 400
    assert r.reconciles


def test_history_bounds_hold_on_quotes_too(tmp_path: Path) -> None:
    ds = tmp_path / "quotes"
    ingest(
        {
            "name": "q",
            "registry": "registry.json",
            "sources": [
                {"format": "binance_bookticker", "symbol": "AAA", "paths": ["bookticker_AAA.csv"]}
            ],
        },
        ds,
        root=FIXTURES,
    )
    name = write_strategy(tmp_path, LOOKAHEAD, "Checker")
    r = run(config(ds, name, {}, tmp_path, execution={"model": "quote"}))
    assert r.status == "completed", r.manifest["error"]
    assert r.strategy.calls == 300


BUY_ONCE = """
class BuyOnce(Strategy):
    def on_quote(self, ctx, quote):
        if ctx.history(quote.symbol, "ts", kind="quotes").size == 1:
            ctx.submit_market(quote.symbol, "buy", "0.001")
"""


def test_quote_fill_reference_is_the_quote_it_filled_against(tmp_path: Path) -> None:
    # bookTicker times are ms: quotes B and C share one. The order (sent at A) fills against B,
    # the first quote after it arrives. Before Phase 9 the analytics took the *last* quote with
    # the fill's timestamp (C) as the reference.
    src = tmp_path / "src"
    src.mkdir()
    (src / "registry.json").write_text((FIXTURES / "registry.json").read_text())
    (src / "q.csv").write_text(
        "update_id,best_bid_price,best_bid_qty,best_ask_price,best_ask_qty,"
        "transaction_time,event_time\n"
        "1,100.00,1,100.02,1,1709251200000,1709251200000\n"  # A
        "2,100.00,1,100.02,1,1709251200001,1709251200001\n"  # B: fills here, mid 100.01
        "3,101.00,1,101.02,1,1709251200001,1709251200001\n"  # C: same ms, mid 101.01
    )
    ds = tmp_path / "ds"
    spec = {
        "name": "q",
        "registry": "registry.json",
        "sources": [{"format": "binance_bookticker", "symbol": "AAA", "paths": ["q.csv"]}],
    }
    ingest(spec, ds, root=src)
    r = run(
        config(
            ds,
            write_strategy(tmp_path, BUY_ONCE, "BuyOnce"),
            {},
            tmp_path,
            execution={"model": "quote"},
        )
    )
    fills = r.read("fills").to_pylist()
    assert [(f["price_real"], f["reference_real"]) for f in fills] == [(100.02, 100.01)]


# --- Orders at the SDK edge ----------------------------------------------------------------------

OFF_GRID = """
class OffGrid(Strategy):
    def on_bar(self, ctx, bar):
        if ctx.bars_seen(bar.symbol) == 1:
            self.tick = ctx.submit_limit(bar.symbol, "buy", "1", "10.005")     # tick 0.01
            self.lot = ctx.submit_market(bar.symbol, "buy", "0.0005")           # lot 0.001
            self.ok = ctx.submit_limit(bar.symbol, "buy", 0.3, 0.1)            # floats, exact
            self.snapped = ctx.round_price(bar.symbol, "10.005", "down")
            self.half_even = ctx.round_price(bar.symbol, "10.005")
            self.up = ctx.round_price(bar.symbol, "10.001", "up")
            # Just above half a tick, past Decimal's 28 digits (rounded to 10.00 before Phase 9).
            self.above_half = ctx.round_price(bar.symbol, "10.0050000000000000000000000001")
"""


def test_off_grid_orders_are_rejected_at_conversion(make_dataset, tmp_path: Path) -> None:
    ds = make_dataset({"AAA": bars_from_closes([10, 10, 10])})
    r = run(config(ds, write_strategy(tmp_path, OFF_GRID, "OffGrid"), {}, tmp_path))
    s = r.strategy
    assert (s.tick.status, s.tick.reject_reason, s.tick.order_id) == ("REJECTED", "OFF_TICK", None)
    assert (s.lot.status, s.lot.reject_reason) == ("REJECTED", "OFF_LOT")
    assert s.ok.order_id == 1 and s.ok.status == "CANCELLED"  # rests at 0.1 until END_OF_RUN
    assert s.ok.cancel_reason == "END_OF_RUN"
    assert s.snapped == Decimal("10.00")
    assert (s.half_even, s.up, s.above_half) == (
        Decimal("10.00"),
        Decimal("10.01"),
        Decimal("10.01"),
    )
    orders = r.read("orders").to_pylist()
    assert [(o["order_id"], o["reject_reason"]) for o in orders] == [
        (1, "NONE"),
        (0, "OFF_TICK"),
        (0, "OFF_LOT"),
    ]
    assert orders[0]["price"] == 10 and orders[0]["qty"] == 300  # 0.1 / 0.01, 0.3 / 0.001
    assert r.manifest["counts"]["sdk_rejected"] == 2


AMEND = """
class Amend(Strategy):
    def on_bar(self, ctx, bar):
        n = ctx.bars_seen(bar.symbol)
        if n == 1:
            self.o = ctx.submit_limit(bar.symbol, "buy", "2", "1")  # far below: never fills
        elif n == 2:
            ctx.amend(self.o, "1", "1")  # reduce in place
        elif n == 3:
            self.rest = self.o.remaining
            self.t = ctx.target_position(bar.symbol, "1")
"""


def test_target_position_counts_amended_quantity(make_dataset, tmp_path: Path) -> None:
    ds = make_dataset({"AAA": bars_from_closes([10, 10, 10, 10])})
    s = run(config(ds, write_strategy(tmp_path, AMEND, "Amend"), {}, tmp_path)).strategy
    assert s.rest == 1.0
    assert s.t is None  # the open buy of 1 already reaches the target


KEEP_CTX = """
import weakref

class KeepCtx(Strategy):
    def on_start(self, ctx):
        self.ctx_ref = weakref.ref(ctx)
"""


def test_a_finished_run_frees_its_engine(make_dataset, tmp_path: Path) -> None:
    # engine -> handler -> context -> engine was a cycle through a C++ member, which Python's GC
    # cannot collect: every run leaked its engine (Phase 9: ~470 MiB per run on 2 M bars).
    import gc

    ds = make_dataset({"AAA": bars_from_closes([10, 11, 12])})
    r = run(config(ds, write_strategy(tmp_path, KEEP_CTX, "KeepCtx"), {}, tmp_path))
    gc.collect()
    assert r.strategy.ctx_ref() is None


def test_exact_conversion_helpers() -> None:
    tick = Grid.parse("0.01")
    assert to_grid("42283.58", tick) == 4228358
    assert to_grid(0.1, tick) == 10 and to_grid(0.3, tick) == 30  # floats via shortest text
    assert to_grid(Decimal("1.005"), tick) is None
    assert to_grid(7, Grid.parse("0.5")) == 14 and to_grid("7.25", Grid.parse("0.5")) is None
    with pytest.raises(TypeError):
        to_grid(True, tick)
    # Beyond Decimal's 28-digit context: still exact (was accepted as 5000000 before Phase 9).
    assert to_grid("50000.000000000000000000000001", tick) is None
    assert to_grid("50000.000000000000000000000000", tick) == 5000000
    assert to_grid("-0.01", tick) == -1  # the engine rejects qty <= 0 (ACC-9), not this helper
    assert to_grid("1e-999999999", tick) is None  # tiny: off grid, and returns at once
    with pytest.raises(ValueError, match="out of range"):
        to_grid("1e40", tick)


# --- Failure handling (task 7) -------------------------------------------------------------------

BOOM = """
class Boom(Strategy):
    def on_bar(self, ctx, bar):
        if ctx.bars_seen(bar.symbol) == 3:
            ctx.submit_market(bar.symbol, "buy", "1")
        if ctx.bars_seen(bar.symbol) == 6:
            raise RuntimeError("boom at bar 6")
"""


def test_strategy_exception_fails_the_run_and_keeps_partial_artefacts(
    make_dataset, tmp_path: Path
) -> None:
    ds = make_dataset({"AAA": bars_from_closes([10 + i for i in range(10)])})
    r = run(
        config(ds, write_strategy(tmp_path, BOOM, "Boom"), {}, tmp_path), run_dir=tmp_path / "f"
    )
    m = r.manifest
    assert r.status == m["status"] == "failed"
    assert m["partial"] is True
    assert m["error"]["type"] == "RuntimeError"
    assert "boom at bar 6" in m["error"]["traceback"] and "on_bar" in m["error"]["traceback"]
    # What happened before the failure is kept: the buy sent at bar 3 filled at bar 4.
    assert r.read("fills").column("price_real").to_pylist() == [13.0]
    assert r.read("equity").num_rows == 6
    assert m["reconciles"]  # the ledger is consistent up to the failure
    assert (
        cli(["backtest", str(_write_yaml(tmp_path, ds, write_strategy(tmp_path, BOOM, "Boom")))])
        == 3
    )


def _write_yaml(tmp_path: Path, ds: Path, strategy: str) -> Path:
    p = tmp_path / "cfg.yaml"
    p.write_text(
        textwrap.dedent(f"""
        dataset: {ds}
        universe: [AAA]
        initial_capital: 1000
        strategy: {{name: "{strategy}"}}
        output_dir: {tmp_path / "cli-runs"}
    """)
    )
    return p


# --- Config (task 3) and CLI (task 5) ------------------------------------------------------------


def test_config_validation(tmp_path: Path) -> None:
    ok = {
        "dataset": "x",
        "universe": ["AAA"],
        "initial_capital": 1000,
        "strategy": {"name": "ma_cross"},
    }
    BacktestConfig.model_validate(ok)
    bad = [
        {**ok, "typo": 1},  # unknown key
        {**ok, "start": "2024-01-01T00:00:00"},  # no timezone (TIME-1)
        {**ok, "fees": {"taker_bps": "0.001"}},  # finer than 1 Rate unit
        {**ok, "initial_capital": "0.000000001"},  # finer than 1 Money unit
        {**ok, "execution": {"model": "quote", "slippage": {"kind": "fixed_bps", "bps": 1}}},
        {**ok, "execution": {"model": "book"}},
        {**ok, "universe": []},
        {**ok, "universe": ["AAA", "AAA"]},
        {**ok, "start": "2024-02-01T00:00:00Z", "end": "2024-01-01T00:00:00Z"},
        {**ok, "execution": {"latency_ns": 0}},  # EVT-3
    ]
    for b in bad:
        with pytest.raises(ValidationError):
            BacktestConfig.model_validate(b)
    c = BacktestConfig.model_validate(
        {
            **ok,
            "fees": {"taker_bps": 0.1},
            "initial_capital": "12.5",
            "start": "2024-03-01T01:00:00+01:00",
        }
    )
    assert c.engine_account()["taker"] == 10 and c.engine_account()["cash"] == 1_250_000_000
    assert c.start_ns == T0_NS  # 01:00+01:00 is 00:00Z


def test_time_window_and_dataset_id(make_dataset, tmp_path: Path) -> None:
    ds = make_dataset({"AAA": bars_from_closes([10] * 20)})
    start = "2024-03-01T00:05:00Z"  # bars 5..14 (open >= start, close <= end)
    end = "2024-03-01T00:15:00Z"
    r = run(config(ds, "ma_cross", {"fast": 2, "slow": 3}, tmp_path, start=start, end=end))
    ts = r.read("equity").column("ts").to_pylist()
    assert ts[0] == T0_NS + 6 * MINUTE_NS and ts[-1] == T0_NS + 15 * MINUTE_NS and len(ts) == 10
    with pytest.raises(ValueError, match="does not match"):
        run(config(ds, "ma_cross", {}, tmp_path, dataset_id="0" * 64))
    with pytest.raises(KeyError):
        run(config(ds, "ma_cross", {}, tmp_path, universe=["NOPE"]))
    with pytest.raises(ValueError, match="unknown parameters"):
        run(config(ds, "ma_cross", {"fastt": 2}, tmp_path))


def test_cli_backtest(make_dataset, tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    ds = make_dataset({"AAA": bars_from_closes([10, 10, 10, 10, 12, 14, 14, 10, 8, 8])})
    cfg = tmp_path / "ma.yaml"
    cfg.write_text(
        textwrap.dedent(f"""
        dataset: {ds}
        universe: [AAA]
        initial_capital: 1000
        fees: {{taker_bps: 10}}
        strategy: {{name: ma_cross, params: {{fast: 2, slow: 3, qty: "1"}}}}
    """)
    )
    assert cli(["backtest", str(cfg), "--run-dir", str(tmp_path / "cli")]) == 0
    out = capsys.readouterr().out
    assert "status       completed" in out and "reconciles   True" in out
    assert (tmp_path / "cli" / "manifest.json").exists()
    assert cli(["backtest", str(cfg), "--run-dir", str(tmp_path / "cli")]) == 1  # exists
    assert cli(["data", "info", str(ds)]) == 0
    assert cli(["nope"]) == 1
