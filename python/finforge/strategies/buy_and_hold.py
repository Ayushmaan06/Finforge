"""Buy and hold: the benchmark every run is compared with (PLAN §5.9)."""

from decimal import ROUND_FLOOR, Decimal
from typing import Any, ClassVar

from finforge.sdk import Bar, Context, Quote, Strategy, params_schema


class BuyAndHold(Strategy):
    """Equal-weight buy and hold of the universe.

    On start, the capital is split equally across the universe. On each instrument's first bar
    (or first quote), it buys the whole lots that `share x (1 - buffer)` pays for at that bar's
    close (or the ask), with one market order, and never trades again. The buffer leaves room for
    the gap to the next open, fees and slippage; whatever still does not fit is cut at fill time
    (OP-1). Same capital, fees, slippage and execution model as the run it benchmarks.

    Params: buffer (fraction of each share kept as cash, default 0.02).
    """

    name = "buy_and_hold"
    defaults: ClassVar[dict[str, Any]] = {"buffer": "0.02"}

    params_schema: ClassVar[dict[str, Any]] = params_schema(
        {
            "buffer": {
                "anyOf": [
                    {"type": "string"},
                    {"type": "number", "minimum": 0, "exclusiveMaximum": 1},
                ]
            },
        }
    )

    def setup(self) -> None:
        self.buffer = Decimal(str(self.params["buffer"]))
        if not 0 <= self.buffer < 1:
            raise ValueError("buffer must be in [0, 1)")
        self.bought: set[str] = set()
        self.share = Decimal(0)

    def on_start(self, ctx: Context) -> None:
        self.share = Decimal(repr(ctx.cash)) / len(ctx.symbols)

    def _buy(self, ctx: Context, symbol: str, price: float) -> None:
        if symbol in self.bought or price <= 0:
            return
        self.bought.add(symbol)
        lot = ctx.instrument(symbol).lot
        lot_value = Decimal(lot.units).scaleb(-lot.digits)
        budget = self.share * (1 - self.buffer)
        lots = (budget / Decimal(repr(price)) / lot_value).to_integral_value(rounding=ROUND_FLOOR)
        if lots > 0:
            ctx.submit_market(symbol, "buy", lots * lot_value)

    def on_bar(self, ctx: Context, bar: Bar) -> None:
        self._buy(ctx, bar.symbol, bar.close)

    def on_quote(self, ctx: Context, quote: Quote) -> None:
        self._buy(ctx, quote.symbol, quote.ask)
