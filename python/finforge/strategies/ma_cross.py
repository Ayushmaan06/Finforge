"""Moving-average crossover."""

from typing import Any, ClassVar

from finforge.sdk import QTY_SCHEMA, Bar, Context, Strategy, params_schema


class MovingAverageCrossover(Strategy):
    """Long when the fast simple moving average of closes crosses above the slow one; flat when it
    crosses back below. Long-only, one fixed position size per instrument.

    Logic, per instrument, on each bar close t (with at least `slow + 1` closes):
        fast_t = mean(close[t-fast+1 .. t]),   slow_t = mean(close[t-slow+1 .. t])
        cross up   (fast_{t-1} <= slow_{t-1} and fast_t > slow_t)  -> buy `qty` if flat
        cross down (fast_{t-1} >= slow_{t-1} and fast_t < slow_t)  -> sell the whole position
    Assumptions: signals use closed bars only (history up to now); orders are market orders and fill
    at the next bar's open (CandleModel CND-1/2), so the strategy pays the overnight gap. No stops.

    Params: fast (bars, default 10), slow (bars, default 50, > fast), qty (instrument units).
    """

    name = "ma_cross"
    defaults: ClassVar[dict[str, Any]] = {"fast": 10, "slow": 50, "qty": "1"}

    params_schema: ClassVar[dict[str, Any]] = params_schema(
        {
            "fast": {"type": "integer", "minimum": 1},
            "slow": {"type": "integer", "minimum": 2},
            "qty": QTY_SCHEMA,
        }
    )

    def setup(self) -> None:
        fast, slow = int(self.params["fast"]), int(self.params["slow"])
        if not 1 <= fast < slow:
            raise ValueError("need 1 <= fast < slow")
        self.fast, self.slow = fast, slow

    def on_bar(self, ctx: Context, bar: Bar) -> None:
        close = ctx.history(bar.symbol, "close", self.slow + 1)
        if len(close) < self.slow + 1:
            return
        fast_now, fast_prev = close[-self.fast :].mean(), close[-self.fast - 1 : -1].mean()
        slow_now, slow_prev = close[-self.slow :].mean(), close[-self.slow - 1 : -1].mean()
        position = ctx.position(bar.symbol)
        if fast_prev <= slow_prev and fast_now > slow_now and position == 0:
            ctx.submit_market(bar.symbol, "buy", self.params["qty"])
        elif fast_prev >= slow_prev and fast_now < slow_now and position > 0:
            ctx.target_position(bar.symbol, 0)
