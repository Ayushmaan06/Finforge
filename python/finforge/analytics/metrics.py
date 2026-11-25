"""Return and risk metrics from an equity curve (PLAN §5.9 conventions).

All inputs are plain numpy arrays. A metric that is undefined for the data (too few returns, zero
volatility, no downside) is `None`, never a made-up number; callers write it as JSON null.

Conventions (recorded in every report):
- returns: simple returns of the equity curve sampled at each UTC day's close (the last point at or
  before the day's end, carried forward over days without points); the first return is measured
  against the initial capital;
- annualisation: `periods_per_year` (default 365, crypto trades every day; 252 for equities);
- risk-free rate: annual, converted per period as (1 + rf)^(1/N) - 1 (default 0);
- Sharpe = mean(excess) / std(excess, ddof=1) x √N (null if undefined; every null metric is
  listed in `undefined`, MET-4); Sortino = mean(excess) / downside x √N with
  downside = √mean(min(r, 0)²) over all periods; volatility = std(r, ddof=1) x √N;
- max drawdown: largest peak-to-trough fall of the full equity curve (initial capital first),
  as a fraction, with its duration;
- VaR / CVaR: historical, 1 period, 95 %, positive loss fractions; the quantile uses linear
  interpolation (numpy's default, the same as empyrical); CVaR averages returns at or below it.
"""

from dataclasses import asdict, dataclass
from typing import Any

import numpy as np

NS_PER_DAY = 86_400_000_000_000


@dataclass(frozen=True)
class Conventions:
    periods_per_year: int = 365
    risk_free_annual: float = 0.0
    var_level: float = 0.95
    resample: str = "UTC daily close"
    quantile_method: str = "linear"

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


DEFAULT_CONVENTIONS = Conventions()


def daily_closes(ts: np.ndarray, equity: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Equity at the close of every UTC day from the first point's day to the last point's day.
    Returns (day close timestamps, values)."""
    ts = np.asarray(ts, dtype=np.int64)
    equity = np.asarray(equity, dtype=np.float64)
    if len(ts) == 0:
        return np.empty(0, np.int64), np.empty(0)
    # A point exactly at midnight belongs to the day that just ended.
    day = (ts - 1) // NS_PER_DAY
    last_of_day = np.flatnonzero(np.r_[day[1:] != day[:-1], True])
    days = np.arange(day[0], day[-1] + 1)
    pos = np.searchsorted(day[last_of_day], days, side="right") - 1  # forward fill
    return (days + 1) * NS_PER_DAY, equity[last_of_day[pos]]


def simple_returns(values: np.ndarray, initial: float) -> np.ndarray:
    v = np.r_[initial, np.asarray(values, dtype=np.float64)]
    return v[1:] / v[:-1] - 1.0


def per_period_rf(conv: Conventions) -> float:
    return (1.0 + conv.risk_free_annual) ** (1.0 / conv.periods_per_year) - 1.0


def sharpe(r: np.ndarray, conv: Conventions = DEFAULT_CONVENTIONS) -> float | None:
    if len(r) < 2:
        return None
    ex = r - per_period_rf(conv)
    sd = float(np.std(ex, ddof=1))
    if sd == 0.0:
        return None
    return float(np.mean(ex)) / sd * np.sqrt(conv.periods_per_year)


def sortino(r: np.ndarray, conv: Conventions = DEFAULT_CONVENTIONS) -> float | None:
    if len(r) < 1:
        return None
    ex = r - per_period_rf(conv)
    downside = float(np.sqrt(np.mean(np.minimum(r, 0.0) ** 2)))
    if downside == 0.0:
        return None
    return float(np.mean(ex)) / downside * np.sqrt(conv.periods_per_year)


def volatility(r: np.ndarray, conv: Conventions = DEFAULT_CONVENTIONS) -> float | None:
    if len(r) < 2:
        return None
    return float(np.std(r, ddof=1)) * np.sqrt(conv.periods_per_year)


def cagr(
    initial: float, final: float, periods: int, conv: Conventions = DEFAULT_CONVENTIONS
) -> float | None:
    if periods <= 0 or initial <= 0 or final <= 0:
        return None
    return (final / initial) ** (conv.periods_per_year / periods) - 1.0


def var_cvar(
    r: np.ndarray, conv: Conventions = DEFAULT_CONVENTIONS
) -> tuple[float | None, float | None]:
    if len(r) == 0:
        return None, None
    q = float(np.quantile(r, 1.0 - conv.var_level, method=conv.quantile_method))
    tail = r[r <= q]
    return -q, -float(np.mean(tail))


def max_drawdown(ts: np.ndarray, equity: np.ndarray, initial: float) -> dict[str, Any]:
    """Largest peak-to-trough fall (fraction) of the curve, initial capital first, with the peak,
    trough and recovery times of that episode, and the longest time spent under water."""
    ts = np.asarray(ts, dtype=np.int64)
    if len(ts) == 0:
        return {
            "max_drawdown": 0.0,
            "peak_ts": None,
            "trough_ts": None,
            "recovery_ts": None,
            "duration_ns": 0,
            "recovered": True,
            "longest_underwater_ns": 0,
        }
    e = np.r_[initial, np.asarray(equity, dtype=np.float64)]
    t = np.r_[ts[0], ts]
    peak = np.maximum.accumulate(e)
    dd = e / peak - 1.0
    trough = int(np.argmin(dd))
    mdd = -float(dd[trough])
    peak_i = int(np.flatnonzero(e[: trough + 1] == peak[trough])[-1]) if mdd > 0 else trough
    after = np.flatnonzero(e[trough:] >= peak[trough])
    recovered = mdd == 0 or len(after) > 0
    end_i = trough + int(after[0]) if len(after) else len(e) - 1
    # Longest run below the running peak, start = last peak before it, end = recovery or last point.
    under = dd < 0
    longest = 0
    start = None
    for i in range(len(e)):
        if under[i] and start is None:
            start = i - 1
        if start is not None and (not under[i] or i == len(e) - 1):
            longest = max(longest, int(t[i] - t[start]))
            start = None
    return {
        "max_drawdown": mdd,
        "peak_ts": int(t[peak_i]) if mdd > 0 else None,
        "trough_ts": int(t[trough]) if mdd > 0 else None,
        "recovery_ts": int(t[end_i]) if mdd > 0 and recovered else None,
        "duration_ns": int(t[end_i] - t[peak_i]) if mdd > 0 else 0,
        "recovered": bool(recovered),
        "longest_underwater_ns": longest,
    }


def performance(
    ts: np.ndarray, equity: np.ndarray, initial: float, conv: Conventions = DEFAULT_CONVENTIONS
) -> dict[str, Any]:
    """Every equity-curve metric of §5.9 (trade metrics come from analytics.trades)."""
    days, closes = daily_closes(ts, equity)
    r = simple_returns(closes, initial)
    final = float(closes[-1]) if len(closes) else initial
    var, cvar = var_cvar(r, conv)
    dd = max_drawdown(ts, equity, initial)
    c = cagr(initial, final, len(r), conv)
    sortino_value = sortino(r, conv)
    # MET-5: no downside but a positive mean excess return is an infinite Sortino (flagged).
    sortino_infinite = (
        sortino_value is None and len(r) > 0 and float(np.mean(r - per_period_rf(conv))) > 0
    )
    out = {
        "initial_equity": initial,
        "final_equity": final,
        "total_return": final / initial - 1.0,
        "cagr": c,
        "volatility": volatility(r, conv),
        "sharpe": sharpe(r, conv),
        "sortino": sortino_value,
        "sortino_infinite": bool(sortino_infinite),
        "max_drawdown": dd["max_drawdown"],
        "drawdown": dd,
        "calmar": (c / dd["max_drawdown"]) if c is not None and dd["max_drawdown"] > 0 else None,
        "var": var,
        "cvar": cvar,
        "periods": len(r),
        "best_period": float(r.max()) if len(r) else None,
        "worst_period": float(r.min()) if len(r) else None,
        "positive_periods": float(np.mean(r > 0)) if len(r) else None,
        "first_day_close_ts": int(days[0]) if len(days) else None,
        "last_day_close_ts": int(days[-1]) if len(days) else None,
    }
    # MET-4: every metric reported as null is listed, so a null is never mistaken for a value.
    out["undefined"] = sorted(
        k
        for k, v in out.items()
        if v is None
        and k
        not in (
            "first_day_close_ts",
            "last_day_close_ts",
            "best_period",
            "worst_period",
            "positive_periods",
        )
    )
    return out
