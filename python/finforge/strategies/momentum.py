"""Time-series momentum."""

from typing import Any, ClassVar

from finforge.sdk import QTY_SCHEMA, Bar, Context, Strategy, params_schema


class TimeSeriesMomentum(Strategy):
    """Hold the instrument while its own past return is positive.

    Logic, per instrument, every `rebalance` bars (counted from its first bar) once
    `lookback + 1` closes exist:
        r = close_t / close_{t-lookback} - 1
        r > 0  -> target +qty;   r <= 0 -> target 0 (or -qty with allow_short)
    The target is reached with one market order for the difference (`ctx.target_position`).
    Assumptions: trends persist over the lookback; rebalancing on a fixed bar count, not on a
    calendar; next-bar fills; no volatility scaling.

    Params: lookback (bars, default 60), rebalance (bars, default 10), qty, allow_short.
    """

    name = "momentum"
    defaults: ClassVar[dict[str, Any]] = {
        "lookback": 60,
        "rebalance": 10,
        "qty": "1",
        "allow_short": False,
    }

    params_schema: ClassVar[dict[str, Any]] = params_schema(
        {
            "lookback": {"type": "integer", "minimum": 1},
            "rebalance": {"type": "integer", "minimum": 1},
            "qty": QTY_SCHEMA,
            "allow_short": {"type": "boolean"},
        }
    )

    def setup(self) -> None:
        self.lookback = int(self.params["lookback"])
        self.rebalance = int(self.params["rebalance"])
        if self.lookback < 1 or self.rebalance < 1:
            raise ValueError("lookback and rebalance must be >= 1")

    def on_bar(self, ctx: Context, bar: Bar) -> None:
        seen = ctx.bars_seen(bar.symbol)
        if seen < self.lookback + 1 or (seen - 1) % self.rebalance != 0:
            return
        close = ctx.history(bar.symbol, "close", self.lookback + 1)
        r = close[-1] / close[0] - 1.0
        qty = self.params["qty"]
        if r > 0:
            ctx.target_position(bar.symbol, qty)
        elif self.params["allow_short"]:
            ctx.target_position(bar.symbol, "-" + str(qty))
        else:
            ctx.target_position(bar.symbol, 0)
