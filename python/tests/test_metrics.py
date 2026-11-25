"""Metrics against hand-computed fixtures (to 1e-9) and against empyrical-reloaded, an independent
library used only here (PLAN Phase 6 task 8, exit criterion 1)."""

import math

import numpy as np
import pytest

from finforge.analytics.metrics import (
    NS_PER_DAY,
    Conventions,
    daily_closes,
    max_drawdown,
    performance,
    sharpe,
    sortino,
    var_cvar,
)

TOL = 1e-9
DAY0 = 19_783 * NS_PER_DAY  # 2024-03-01T00:00Z


def at_close(day: int, hours: float = 24.0) -> int:
    return DAY0 + day * NS_PER_DAY + int(hours * 3_600_000_000_000)


# Returns r = [+1 %, -1 %, +3 %, -2 %, 0] from 100, one point per day at the day's close.
R = [0.01, -0.01, 0.03, -0.02, 0.0]
CLOSES = [100.0]
for x in R:
    CLOSES.append(CLOSES[-1] * (1 + x))
TS = np.array([at_close(d) for d in range(5)])
EQ = np.array(CLOSES[1:])


def test_hand_computed_fixture() -> None:
    p = performance(TS, EQ, 100.0)
    # mean = 0.01 / 5 = 0.002; deviations .008 -.012 .028 -.022 -.002; Σ squares = 1.48e-3;
    # sample variance (ddof 1) = 1.48e-3 / 4 = 3.7e-4.
    assert p["periods"] == 5
    assert p["sharpe"] == pytest.approx(0.002 / math.sqrt(3.7e-4) * math.sqrt(365), abs=TOL)
    assert p["volatility"] == pytest.approx(math.sqrt(3.7e-4) * math.sqrt(365), abs=TOL)
    # Downside = √((0.01² + 0.02²) / 5) = √1e-4 = 0.01 over all 5 periods.
    assert p["sortino"] == pytest.approx(0.002 / 0.01 * math.sqrt(365), abs=TOL)
    # Peak 102.9897 (day 2) to 100.929906 (day 3): exactly -2 %, never recovered.
    assert p["max_drawdown"] == pytest.approx(0.02, abs=TOL)
    dd = p["drawdown"]
    assert (dd["peak_ts"], dd["trough_ts"], dd["recovered"]) == (TS[2], TS[3], False)
    # Linear quantile at 5 %: sorted [-.02, -.01, 0, .01, .03], position 0.2 → -0.018.
    assert p["var"] == pytest.approx(0.018, abs=TOL)
    assert p["cvar"] == pytest.approx(0.02, abs=TOL)  # only -0.02 is at or below -0.018
    total = 1.01 * 0.99 * 1.03 * 0.98
    assert p["total_return"] == pytest.approx(total - 1, abs=TOL)
    assert p["cagr"] == pytest.approx(total ** (365 / 5) - 1, abs=TOL)
    assert p["calmar"] == pytest.approx((total ** (365 / 5) - 1) / 0.02, abs=TOL)
    assert p["best_period"] == pytest.approx(0.03, abs=TOL)
    assert p["worst_period"] == pytest.approx(-0.02, abs=TOL)
    assert p["positive_periods"] == pytest.approx(0.4)


def test_risk_free_rate_is_converted_per_period() -> None:
    conv = Conventions(risk_free_annual=0.05)
    rf = 1.05 ** (1 / 365) - 1
    ex = np.array(R) - rf
    expected = ex.mean() / ex.std(ddof=1) * math.sqrt(365)  # std of excess == std of r
    assert sharpe(np.array(R), conv) == pytest.approx(expected, abs=TOL)
    assert sortino(np.array(R), conv) == pytest.approx(ex.mean() / 0.01 * math.sqrt(365), abs=TOL)
    assert Conventions(periods_per_year=252).to_dict()["periods_per_year"] == 252


def test_daily_resampling_uses_the_last_point_and_forward_fills() -> None:
    ts = np.array(
        [
            at_close(0, 1),
            at_close(0, 23),  # day 0: two points, the later one counts
            at_close(0),  # exactly midnight: still day 0's close
            at_close(2, 5),  # day 1 has no point: carried forward
        ]
    )
    eq = np.array([1.0, 2.0, 3.0, 4.0])
    days, closes = daily_closes(ts, eq)
    assert days.tolist() == [at_close(0), at_close(1), at_close(2)]
    assert closes.tolist() == [3.0, 3.0, 4.0]


def test_drawdown_duration_and_recovery() -> None:
    hour = 3_600_000_000_000
    ts = np.arange(1, 8) * hour
    eq = np.array([110, 100, 99, 105, 111, 90, 95], dtype=float)  # initial 100
    dd = max_drawdown(ts, eq, 100.0)
    # Peaks: 110 (h1) → 99 is -10 % (recovered at 111, h5); 111 (h5) → 90 is -18.9 %, unrecovered.
    assert dd["max_drawdown"] == pytest.approx(1 - 90 / 111, abs=TOL)
    assert (dd["peak_ts"], dd["trough_ts"], dd["recovery_ts"]) == (5 * hour, 6 * hour, None)
    assert dd["duration_ns"] == 2 * hour and not dd["recovered"]
    # Longest under water: h1 → h5 (4 hours); the last episode h5 → h7 is 2 hours.
    assert dd["longest_underwater_ns"] == 4 * hour


def test_undefined_metrics_are_none_not_numbers() -> None:
    flat = performance(TS, np.full(5, 100.0), 100.0)
    assert flat["sharpe"] is None and flat["sortino"] is None and flat["calmar"] is None
    assert flat["undefined"] == ["calmar", "sharpe", "sortino"] and not flat["sortino_infinite"]
    # Only gains: no downside, positive mean -> Sortino infinite (flagged, MET-5).
    up = performance(TS, np.array([101.0, 103.0, 103.5, 104.0, 106.0]), 100.0)
    assert up["sortino"] is None and up["sortino_infinite"] is True
    assert up["sharpe"] is not None and "sortino" in up["undefined"]
    assert flat["volatility"] == 0.0 and flat["max_drawdown"] == 0.0
    one = performance(TS[:1], EQ[:1], 100.0)
    assert one["sharpe"] is None and one["volatility"] is None
    empty = performance(np.empty(0, np.int64), np.empty(0), 100.0)
    assert empty["periods"] == 0 and empty["var"] is None and empty["total_return"] == 0.0
    assert var_cvar(np.empty(0)) == (None, None)


def test_cross_check_against_empyrical() -> None:
    empyrical = pytest.importorskip("empyrical")
    rng = np.random.default_rng(2026)
    r = rng.normal(0.0005, 0.02, 500)
    r[0] = abs(r[0])  # empyrical's drawdown starts its peak at the first return, not the capital
    closes = 1000.0 * np.cumprod(1 + r)
    ts = np.array([at_close(d) for d in range(len(r))])
    p = performance(ts, closes, 1000.0)
    ann = {"annualization": 365}
    assert p["sharpe"] == pytest.approx(empyrical.sharpe_ratio(r, **ann), abs=TOL)
    assert p["sortino"] == pytest.approx(empyrical.sortino_ratio(r, **ann), abs=TOL)
    assert p["volatility"] == pytest.approx(empyrical.annual_volatility(r, **ann), abs=TOL)
    assert p["max_drawdown"] == pytest.approx(-empyrical.max_drawdown(r), abs=TOL)
    assert p["cagr"] == pytest.approx(empyrical.cagr(r, **ann), abs=TOL)
    assert p["var"] == pytest.approx(-empyrical.value_at_risk(r, cutoff=0.05), abs=TOL)
    assert p["cvar"] == pytest.approx(-empyrical.conditional_value_at_risk(r, cutoff=0.05), abs=TOL)
    assert p["calmar"] == pytest.approx(empyrical.calmar_ratio(r, **ann), abs=TOL)
    rf = 0.03
    conv = Conventions(risk_free_annual=rf)
    rf_p = (1 + rf) ** (1 / 365) - 1
    assert sharpe(r, conv) == pytest.approx(
        empyrical.sharpe_ratio(r, risk_free=rf_p, **ann), abs=TOL
    )
