"""Trade analytics: FIFO round trips from fills (PLAN §5.9: a trade is a round trip built by FIFO
matching of fills per instrument).

One trade record per matched piece: an opening fill's remaining lots matched, oldest first,
against a closing fill. A fill larger than the open position closes it and opens the rest in the
other direction (a flip). Gross P&L is exact (integer Money); each fill's fee and execution cost are
split between the pieces it takes part in, pro rata by quantity.

Execution cost of a fill = fee + adverse difference between the fill price and its reference price
(Candle: the bar open before slippage; Quote: the quote mid), so it is fees plus slippage / spread.
A limit order filled better than the reference has a negative price cost (price improvement).
"""

from collections import deque
from dataclasses import dataclass
from typing import Any

import numpy as np

MONEY = 100_000_000


@dataclass
class _Lot:
    side: int  # +1 long, -1 short
    qty: int  # lots still open
    qty0: int  # lots of the opening fill
    price: int  # ticks
    price_real: float
    ts: int
    fee: int  # Money of the whole opening fill
    cost_per_lot: float  # price cost per lot of the opening fill (quote currency)


def fifo_trades(
    fills: list[dict[str, Any]], units: dict[str, tuple[int, int, float]]
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    """Round trips and the lots still open.

    `fills`: fills.parquet rows in time order (ts, symbol, side, price, qty, fee, price_real,
    qty_real, reference_real). `units`: symbol -> (money_per_tick_lot, contract_multiplier,
    lot size as a float)."""
    books: dict[str, deque[_Lot]] = {}
    trades: list[dict[str, Any]] = []
    for f in fills:
        sym = f["symbol"]
        mptl, mult, lot_size = units[sym]
        side = 1 if f["side"] == "buy" else -1
        qty, price, fee = int(f["qty"]), int(f["price"]), int(f["fee"])
        ref = f.get("reference_real")
        # Adverse price difference in quote currency, per lot of this fill.
        cost_per_lot = (
            0.0
            if ref is None or np.isnan(ref)
            else (side * (f["price_real"] - ref) * lot_size * mult)
        )
        book = books.setdefault(sym, deque())
        left = qty
        while left and book and book[0].side == -side:
            lot = book[0]
            q = min(left, lot.qty)
            gross = lot.side * (price - lot.price) * q * mptl * mult  # Money, exact
            fees = lot.fee * q / lot.qty0 + fee * q / qty  # pro rata (Money)
            price_cost = lot.cost_per_lot * q + cost_per_lot * q
            trades.append(
                {
                    "symbol": sym,
                    "direction": "long" if lot.side == 1 else "short",
                    "entry_ts": lot.ts,
                    "exit_ts": int(f["ts"]),
                    "duration_ns": int(f["ts"]) - lot.ts,
                    "qty": q * lot_size,
                    "entry_price": lot.price_real,
                    "exit_price": float(f["price_real"]),
                    "gross_pnl": gross / MONEY,
                    "fees": fees / MONEY,
                    "net_pnl": (gross - fees) / MONEY,
                    "price_cost": price_cost,
                    "execution_cost": fees / MONEY + price_cost,
                }
            )
            lot.qty -= q
            left -= q
            if lot.qty == 0:
                book.popleft()
        if left:  # opens, adds to, or flips into a position. qty0 = the whole fill, so a flip's
            # fee is shared between the pieces it closed above and these remaining lots.
            book.append(
                _Lot(
                    side, left, qty, price, float(f["price_real"]), int(f["ts"]), fee, cost_per_lot
                )
            )
    open_lots = [
        {
            "symbol": s,
            "direction": "long" if lot.side == 1 else "short",
            "entry_ts": lot.ts,
            "qty": lot.qty * units[s][2],
            "entry_price": lot.price_real,
        }
        for s, book in books.items()
        for lot in book
    ]
    return trades, open_lots


def trade_summary(trades: list[dict[str, Any]], open_lots: list[dict[str, Any]]) -> dict[str, Any]:
    """Win rate, profit factor (net of fees), P&L and cost totals, holding-period distribution."""
    net = np.array([t["net_pnl"] for t in trades], dtype=np.float64)
    wins, losses = net[net > 0], net[net < 0]
    gross_profit, gross_loss = float(wins.sum()), float(-losses.sum())
    infinite = len(trades) > 0 and gross_loss == 0.0 and gross_profit > 0
    hold = np.array([t["duration_ns"] for t in trades], dtype=np.float64) / 1e9
    holding = None
    if len(hold):
        p = np.percentile(hold, [0, 25, 50, 75, 100])
        holding = {
            "min_s": p[0],
            "p25_s": p[1],
            "median_s": p[2],
            "p75_s": p[3],
            "max_s": p[4],
            "mean_s": float(hold.mean()),
        }
    return {
        "count": len(trades),
        "wins": len(wins),
        "losses": len(losses),
        "win_rate": float(len(wins) / len(trades)) if trades else None,
        "profit_factor": (gross_profit / gross_loss) if gross_loss > 0 else None,
        "profit_factor_infinite": bool(infinite),
        "gross_profit": gross_profit,
        "gross_loss": gross_loss,
        "net_pnl": float(net.sum()),
        "average_net_pnl": float(net.mean()) if len(net) else None,
        "fees": float(sum(t["fees"] for t in trades)),
        "price_cost": float(sum(t["price_cost"] for t in trades)),
        "execution_cost": float(sum(t["execution_cost"] for t in trades)),
        "holding_period": holding,
        "open_lots": len(open_lots),
    }
