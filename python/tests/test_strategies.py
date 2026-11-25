"""Reference strategies on crafted fixtures: signals fire exactly on the bars computed by hand
(PLAN Phase 5 exit criterion 1), and every run reconciles (exit criterion 4)."""

from pathlib import Path

import pytest
from conftest import MINUTE_NS, T0_NS, bars_from_closes, config

from finforge.backtest import run


def sent_and_filled(result) -> list[tuple[str, str, int, float]]:
    """(symbol, side, bar index the order was sent on, fill price) per filled order."""
    orders = result.read("orders").to_pylist()
    fills = {f["order_id"]: f for f in result.read("fills").to_pylist()}
    out = []
    for o in orders:
        if o["order_id"] in fills:
            bar = (o["sent_ts"] - T0_NS) // MINUTE_NS - 1  # sent at bar i's close = T0 + (i+1) min
            out.append((o["symbol"], o["side"], bar, fills[o["order_id"]]["price_real"]))
    return out


def test_ma_cross_signals(make_dataset, tmp_path: Path) -> None:
    # SMA2 vs SMA3 (hand-computed):
    #   t=4: fast 11 > slow 10.67, previously 10 <= 10      -> cross up: buy (fills at bar 5 = 14)
    #   t=7: fast 12 < slow 12.67, previously 14 >= 13.33   -> cross down: sell (bar 8 = 8)
    closes = [10, 10, 10, 10, 12, 14, 14, 10, 8, 8]
    ds = make_dataset({"AAA": bars_from_closes(closes)})
    r = run(config(ds, "ma_cross", {"fast": 2, "slow": 3, "qty": "1.5"}, tmp_path))
    assert r.status == "completed" and r.reconciles
    assert sent_and_filled(r) == [("AAA", "buy", 4, 14.0), ("AAA", "sell", 7, 8.0)]
    # Long 1.5 from 14 to 8: -9 USDT, no fees configured.
    assert r.manifest["summary"]["realised_pnl"] == pytest.approx(-9.0)


def test_mean_reversion_signals(make_dataset, tmp_path: Path) -> None:
    # lookback 5: t=5 window [100,100,100,100,90], mean 98, std 4 -> z = -2 < -1.5: buy.
    # t=6 window [100,100,100,90,100] -> z = +0.5 > -0.5: exit. Before t=5, std = 0: no signal.
    closes = [100, 100, 100, 100, 100, 90, 100, 100, 100, 100]
    ds = make_dataset({"AAA": bars_from_closes(closes)})
    params = {"lookback": 5, "entry_z": 1.5, "exit_z": 0.5, "qty": "2"}
    r = run(config(ds, "mean_reversion", params, tmp_path))
    assert r.status == "completed" and r.reconciles
    assert sent_and_filled(r) == [("AAA", "buy", 5, 100.0), ("AAA", "sell", 6, 100.0)]


def test_momentum_signals(make_dataset, tmp_path: Path) -> None:
    # lookback 3, rebalance every 2 bars from the 4th bar: decisions at t = 4, 6, 8.
    #   t=4: 14/11 - 1 > 0 -> target +1: buy.  t=6: 12/13 - 1 < 0 -> target 0: sell.
    #   t=8: 10/11 - 1 < 0 -> already flat: nothing.
    closes = [10, 11, 12, 13, 14, 13, 12, 11, 10, 9]
    ds = make_dataset({"AAA": bars_from_closes(closes)})
    r = run(config(ds, "momentum", {"lookback": 3, "rebalance": 2, "qty": "1"}, tmp_path))
    assert r.status == "completed" and r.reconciles
    assert sent_and_filled(r) == [("AAA", "buy", 4, 13.0), ("AAA", "sell", 6, 11.0)]


def test_pairs_signals(make_dataset, tmp_path: Path) -> None:
    # Spread log(A) - log(B), lookback 4. t=4: last spreads [0,0,0,log 1.2] -> z = +1.73 > 1.5:
    # short A, long B. t=5: [0,0,log 1.2,0] -> z = -0.577, |z| < 0.6: close both.
    a = [100, 100, 100, 100, 120, 100, 100, 100, 100, 100]
    b = [100] * 10
    ds = make_dataset({"AAA": bars_from_closes(a), "BBB": bars_from_closes(b)})
    params = {
        "a": "AAA",
        "b": "BBB",
        "lookback": 4,
        "entry_z": 1.5,
        "exit_z": 0.6,
        "qty_a": "1",
        "qty_b": "1",
    }
    r = run(
        config(
            ds,
            "pairs",
            params,
            tmp_path,
            universe=["AAA", "BBB"],
            risk={"allow_short": True, "max_leverage": 2},
        )
    )
    assert r.status == "completed" and r.reconciles
    expected = sorted(
        [
            ("AAA", "sell", 4, 100.0),
            ("BBB", "buy", 4, 100.0),
            ("AAA", "buy", 5, 100.0),
            ("BBB", "sell", 5, 100.0),
        ]
    )
    assert sorted(sent_and_filled(r)) == expected
    # Same signals when the legs are given the other way round: B (AAA, id 0) now arrives first.
    flipped = {**params, "a": "BBB", "b": "AAA"}
    ds2 = make_dataset({"AAA": bars_from_closes(b), "BBB": bars_from_closes(a)}, name="flipped")
    r2 = run(
        config(
            ds2,
            "pairs",
            flipped,
            tmp_path,
            universe=["AAA", "BBB"],
            risk={"allow_short": True, "max_leverage": 2},
        )
    )
    assert sorted(sent_and_filled(r2)) == sorted(
        [("BBB" if s == "AAA" else "AAA", side, bar, px) for s, side, bar, px in expected]
    )


@pytest.mark.parametrize(
    ("name", "params"),
    [
        ("ma_cross", {"fast": 5, "slow": 20, "qty": "0.5"}),
        (
            "mean_reversion",
            {"lookback": 20, "entry_z": 1.0, "exit_z": 0.2, "qty": "0.5", "allow_short": True},
        ),
        ("momentum", {"lookback": 10, "rebalance": 5, "qty": "0.5", "allow_short": True}),
    ],
)
def test_reference_strategies_reconcile_on_the_fixture_sample(name, params, tmp_path: Path) -> None:
    """The committed fixture klines (300 bars of AAA), with fees, slippage and shorts."""
    from finforge.data import ingest

    fixtures = Path(__file__).resolve().parents[2] / "datasets" / "fixtures"
    ds = tmp_path / "ds"
    ingest(
        {
            "name": "fixture",
            "registry": "registry.json",
            "sources": [
                {
                    "format": "binance_klines",
                    "symbol": "AAA",
                    "interval": "1m",
                    "paths": ["klines_AAA_ms.csv"],
                },
            ],
        },
        ds,
        root=fixtures,
    )
    cfg = config(
        ds,
        name,
        params,
        tmp_path,
        execution={"model": "candle", "slippage": {"kind": "fixed_bps", "bps": 5}},
        fees={"maker_bps": 2, "taker_bps": 10, "min_fee": "0.01"},
        risk={"allow_short": True, "max_leverage": 2},
    )
    r = run(cfg)
    assert r.status == "completed"
    assert r.reconciles
    assert r.manifest["counts"]["fills"] > 0
