"""Pairs trading on the z-score of a log-price spread (optional reference strategy)."""

from typing import Any, ClassVar

import numpy as np

from finforge.sdk import QTY_SCHEMA, Bar, Context, Strategy, params_schema


class PairsTrading(Strategy):
    """Trade the spread s = log(A) - beta·log(B) of two instruments back to its rolling mean.

    Logic, once per close time, on whichever leg's bar arrives second (bars with equal close times
    arrive in instrument-id order, EVT-2), once both have `lookback` closes:
        z = (s_t - mean(s)) / std(s) over the last `lookback` bars
        flat and z >  entry_z -> short A `qty_a`, long B `qty_b`   (spread expected to fall)
        flat and z < -entry_z -> long A, short B
        open and |z| < exit_z -> close both legs
    Assumptions: A and B are cointegrated with a fixed hedge ratio `beta` (default 1); both legs
    fill at the next bar's open; requires `risk.allow_short`. Quantities are fixed, not
    dollar-neutral.

    Params: a, b (symbols), lookback (default 60), entry_z (2.0), exit_z (0.5), beta (1.0),
    qty_a, qty_b.
    """

    name = "pairs"
    defaults: ClassVar[dict[str, Any]] = {
        "a": "",
        "b": "",
        "lookback": 60,
        "entry_z": 2.0,
        "exit_z": 0.5,
        "beta": 1.0,
        "qty_a": "1",
        "qty_b": "1",
    }

    params_schema: ClassVar[dict[str, Any]] = params_schema(
        {
            "a": {"type": "string", "minLength": 1},
            "b": {"type": "string", "minLength": 1},
            "lookback": {"type": "integer", "minimum": 2},
            "entry_z": {"type": "number", "exclusiveMinimum": 0},
            "exit_z": {"type": "number", "minimum": 0},
            "beta": {"type": "number"},
            "qty_a": QTY_SCHEMA,
            "qty_b": QTY_SCHEMA,
        },
        required=["a", "b"],
    )

    def setup(self) -> None:
        if not self.params["a"] or not self.params["b"] or self.params["a"] == self.params["b"]:
            raise ValueError("pairs needs two different symbols a and b")
        self.lookback = int(self.params["lookback"])
        self.entry, self.exit = float(self.params["entry_z"]), float(self.params["exit_z"])
        self.beta = float(self.params["beta"])
        if self.lookback < 2 or not 0 <= self.exit < self.entry:
            raise ValueError("need lookback >= 2 and 0 <= exit_z < entry_z")
        self.state = 0  # +1 long spread, -1 short spread, 0 flat

    def on_bar(self, ctx: Context, bar: Bar) -> None:
        a, b = self.params["a"], self.params["b"]
        if bar.symbol not in (a, b):
            return
        ta, tb = ctx.history(a, "ts", 1), ctx.history(b, "ts", 1)
        if len(ta) == 0 or len(tb) == 0 or not ta[0] == tb[0] == bar.ts:
            return  # the other leg's bar for this close has not arrived yet
        pa, pb = ctx.history(a, "close", self.lookback), ctx.history(b, "close", self.lookback)
        if len(pa) < self.lookback or len(pb) < self.lookback:
            return
        spread = np.log(pa) - self.beta * np.log(pb)
        std = float(np.std(spread))
        if std == 0.0:
            return
        z = (spread[-1] - float(np.mean(spread))) / std
        qa, qb = str(self.params["qty_a"]), str(self.params["qty_b"])
        if self.state == 0 and z > self.entry:
            ctx.target_position(a, "-" + qa)
            ctx.target_position(b, qb)
            self.state = -1
        elif self.state == 0 and z < -self.entry:
            ctx.target_position(a, qa)
            ctx.target_position(b, "-" + qb)
            self.state = 1
        elif self.state != 0 and abs(z) < self.exit:
            ctx.target_position(a, 0)
            ctx.target_position(b, 0)
            self.state = 0
