"""Mean reversion on the z-score of price against its rolling mean."""

from typing import Any, ClassVar

import numpy as np

from finforge.sdk import QTY_SCHEMA, Bar, Context, Strategy, params_schema


class MeanReversion(Strategy):
    """Buy when price is unusually far below its rolling mean; exit when it has reverted.

    Logic, per instrument, on each bar close with at least `lookback` closes:
        z = (close_t - mean(last `lookback` closes)) / std(last `lookback` closes, ddof=0)
        flat and z < -entry_z  -> buy `qty`
        long and z > -exit_z   -> sell the whole position
        (with allow_short: the mirror image above +entry_z, covered below +exit_z)
    Assumptions: stationarity of price around a slowly moving mean over the lookback; no
    trend filter; a zero standard deviation means no signal. Market orders, next-bar fills.

    Params: lookback (bars, default 20), entry_z (default 2.0), exit_z (default 0.5, < entry_z),
    qty (instrument units), allow_short (default False).
    """

    name = "mean_reversion"
    defaults: ClassVar[dict[str, Any]] = {
        "lookback": 20,
        "entry_z": 2.0,
        "exit_z": 0.5,
        "qty": "1",
        "allow_short": False,
    }

    params_schema: ClassVar[dict[str, Any]] = params_schema(
        {
            "lookback": {"type": "integer", "minimum": 2},
            "entry_z": {"type": "number", "exclusiveMinimum": 0},
            "exit_z": {"type": "number", "minimum": 0},
            "qty": QTY_SCHEMA,
            "allow_short": {"type": "boolean"},
        }
    )

    def setup(self) -> None:
        self.lookback = int(self.params["lookback"])
        self.entry, self.exit = float(self.params["entry_z"]), float(self.params["exit_z"])
        if self.lookback < 2 or not 0 <= self.exit < self.entry:
            raise ValueError("need lookback >= 2 and 0 <= exit_z < entry_z")

    def on_bar(self, ctx: Context, bar: Bar) -> None:
        close = ctx.history(bar.symbol, "close", self.lookback)
        if len(close) < self.lookback:
            return
        std = float(np.std(close))
        if std == 0.0:
            return
        z = (close[-1] - float(np.mean(close))) / std
        pos = ctx.position(bar.symbol)
        qty = self.params["qty"]
        if pos == 0 and not ctx.open_orders(bar.symbol):
            if z < -self.entry:
                ctx.submit_market(bar.symbol, "buy", qty)
            elif z > self.entry and self.params["allow_short"]:
                ctx.submit_market(bar.symbol, "sell", qty)
        elif (pos > 0 and z > -self.exit) or (pos < 0 and z < self.exit):
            ctx.target_position(bar.symbol, 0)
