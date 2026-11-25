"""Exposure analytics and P&L per instrument (PLAN Phase 6 task 3).

Inputs are the engine's equity snapshots with each instrument's position (lots) and mark (ticks).
Per point: gross = Σ|value_i|, net = Σ value_i, concentration = max_i |value_i| / gross and the
Herfindahl index Σ (|value_i| / gross)², capital utilisation = gross / equity.

P&L per instrument is computed from cash flows, exactly in Money: -Σ signed notional of its fills -
its fees + its final market value. Summed over instruments it equals the change in equity plus
borrow costs (borrow is charged to the account, not to an instrument).
"""

from typing import Any

import numpy as np

MONEY = 100_000_000


def exposure(
    equity: np.ndarray, position: np.ndarray, mark: np.ndarray, unit_value: np.ndarray
) -> dict[str, np.ndarray]:
    """Per-point series. `position`, `mark`: (points, instruments) ints; `unit_value`: quote
    currency per (lot x tick) per instrument (money_per_tick_lot x multiplier / 1e8)."""
    value = position.astype(np.float64) * mark.astype(np.float64) * unit_value
    gross = np.abs(value).sum(axis=1)
    net = value.sum(axis=1)
    with np.errstate(invalid="ignore", divide="ignore"):
        share = np.where(gross[:, None] > 0, np.abs(value) / gross[:, None], 0.0)
        utilisation = np.where(equity > 0, gross / equity, np.nan)
    return {
        "value": value,
        "gross": gross,
        "net": net,
        "max_share": share.max(axis=1) if share.size else np.zeros(len(gross)),
        "hhi": (share**2).sum(axis=1),
        "utilisation": utilisation,
    }


def exposure_summary(series: dict[str, np.ndarray], equity: np.ndarray) -> dict[str, Any]:
    gross, net = series["gross"], series["net"]
    if len(gross) == 0:
        return {"points": 0}
    invested = gross > 0
    with np.errstate(invalid="ignore", divide="ignore"):
        net_frac = np.where(equity > 0, net / equity, np.nan)
    util = series["utilisation"]
    return {
        "points": len(gross),
        "time_in_market": float(invested.mean()),
        "mean_gross": float(gross.mean()),
        "max_gross": float(gross.max()),
        "mean_utilisation": float(np.nanmean(util)) if np.isfinite(util).any() else None,
        "max_utilisation": float(np.nanmax(util)) if np.isfinite(util).any() else None,
        "mean_net_fraction": float(np.nanmean(net_frac)) if np.isfinite(net_frac).any() else None,
        "mean_max_share_when_invested": float(series["max_share"][invested].mean())
        if invested.any()
        else None,
        "mean_hhi_when_invested": float(series["hhi"][invested].mean()) if invested.any() else None,
    }


def pnl_by_instrument(
    fills: list[dict[str, Any]],
    final_position: dict[str, int],
    final_mark: dict[str, int],
    units: dict[str, tuple[int, int, float]],
) -> dict[str, dict[str, Any]]:
    """Exact cash-flow P&L per symbol (quote currency), with fees and fill counts."""
    out: dict[str, dict[str, Any]] = {}
    for sym in final_position:
        out[sym] = {"cash_flow": 0, "fees": 0, "fills": 0}
    for f in fills:
        mptl, mult, _ = units[f["symbol"]]
        n = int(f["price"]) * int(f["qty"]) * mptl * mult
        o = out.setdefault(f["symbol"], {"cash_flow": 0, "fees": 0, "fills": 0})
        o["cash_flow"] += -n if f["side"] == "buy" else n
        o["fees"] += int(f["fee"])
        o["fills"] += 1
    result = {}
    for sym, o in out.items():
        mptl, mult, lot_size = units[sym]
        mv = final_position.get(sym, 0) * final_mark.get(sym, 0) * mptl * mult
        pnl = o["cash_flow"] - o["fees"] + mv
        result[sym] = {
            "pnl": pnl / MONEY,
            "pnl_money": pnl,
            "fees": o["fees"] / MONEY,
            "fills": o["fills"],
            "final_position": final_position.get(sym, 0) * lot_size,
            "final_market_value": mv / MONEY,
        }
    return result
