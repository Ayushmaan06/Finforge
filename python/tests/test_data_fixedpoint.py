"""Exact decimal → fixed-point conversion (NUM-1, INS-1)."""

import random
from decimal import Decimal

import numpy as np
import pyarrow as pa
import pytest

import finforge
from finforge.data import Grid, Registry, parse_on_grid, parse_scaled


def test_grid_parse() -> None:
    assert Grid.parse("0.01") == Grid("0.01", 1, 2)
    assert Grid.parse("0.01000000") == Grid("0.01000000", 1, 2)
    assert Grid.parse("0.5") == Grid("0.5", 5, 1)
    assert Grid.parse("10") == Grid("10", 10, 0)
    for bad in ["0", "0.000", "-0.01", "1e-2", "", ".5", "abc"]:
        with pytest.raises(ValueError):
            Grid.parse(bad)


def test_parse_scaled_cases() -> None:
    col = pa.array(
        ["12.3400", " 5 ", "-0.5", "abc", "", None, "1.", "007.10", "0.000", "1.239", "1e5"]
    )
    p = parse_scaled(col, 2)
    assert p.values.tolist() == [1234, 500, -50, 0, 0, 0, 100, 710, 0, 0, 0]
    assert p.missing.tolist() == [0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0]
    assert p.bad.tolist() == [0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1]
    assert p.off_grid.tolist() == [0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0]


def test_parse_on_grid_ticks_and_off_tick() -> None:
    half = Grid.parse("0.5")
    p = parse_on_grid(pa.array(["2500.50000000", "2500.0", "2500.25", "3", "-1.5"]), half)
    assert p.values.tolist() == [5001, 5000, 0, 6, -3]
    assert p.off_grid.tolist() == [False, False, True, False, False]


def test_int64_overflow_is_bad_not_wrapped() -> None:
    p = parse_scaled(pa.array(["92233720368.54775807", "92233720368.54775808", "1e3"]), 8)
    assert p.values.tolist() == [2**63 - 1, 0, 0]
    assert p.bad.tolist() == [False, True, True]
    ns = parse_scaled(pa.array(["1709251200000000000", "-5"]), 0)  # 19-digit ns timestamps fit
    assert ns.values.tolist() == [1_709_251_200_000_000_000, -5]


def test_floats_go_through_their_shortest_text() -> None:
    # Generic Parquet sources may hold doubles; they are converted via the shortest round-trip
    # text, so 0.1 becomes exactly 10 ticks of 0.01, never 9.
    p = parse_on_grid(pa.array([0.1, 42283.58, 0.3, 1e-5]), Grid.parse("0.01"))
    assert p.values.tolist() == [10, 4228358, 30, 0]
    assert p.off_grid.tolist() == [False, False, False, True]


def test_random_decimals_match_python_decimal() -> None:
    rng = random.Random(7)
    texts, want = [], []
    for _ in range(5000):
        whole = rng.randint(0, 10**9)
        frac = "".join(rng.choice("0123456789") for _ in range(rng.randint(0, 8)))
        sign = rng.choice(["", "-"])
        t = f"{sign}{whole}" + (f".{frac}" if frac else "")
        texts.append(t)
        want.append(Decimal(t) * 10**8)
    p = parse_scaled(pa.array(texts), 8)
    assert not p.bad.any() and not p.off_grid.any()
    assert p.values.tolist() == [int(w) for w in want]
    assert p.values.dtype == np.int64


def test_registry_uses_engine_num2_rule() -> None:
    assert finforge._engine.money_per_tick_lot("0.01", "0.00001") == 10
    base = {"symbol": "X", "quote_currency": "USDT", "tick_size": "0.01", "lot_size": "0.001"}
    reg = Registry.from_dicts([base, {**base, "symbol": "Y", "tick_size": "0.5", "lot_size": "1"}])
    assert [i.instrument_id for i in reg] == [0, 1]
    with pytest.raises(ValueError):  # 1e-5 * 1e-5 / 1e-8 is not an integer (NUM-2)
        Registry.from_dicts([{**base, "tick_size": "0.00001", "lot_size": "0.00001"}])
    with pytest.raises(ValueError):  # floats are refused (INS-1)
        Registry.from_dicts([{**base, "tick_size": 0.01}])
    with pytest.raises(ValueError):
        Registry.from_dicts([base, base])  # duplicate symbol
    with pytest.raises(ValueError):
        Registry.from_dicts([{**base, "instrument_id": 3}])  # not dense
