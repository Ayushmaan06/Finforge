"""Strategy SDK (PLAN Phase 5): the API a Python strategy is written against.

A strategy subclasses `Strategy` and reacts to callbacks; everything it can do goes through the
`Context` it is handed:

    class Breakout(Strategy):
        def on_bar(self, ctx, bar):
            highs = ctx.history(bar.symbol, "high", 20)
            if len(highs) == 20 and bar.close > highs[:-1].max() and ctx.position(bar.symbol) == 0:
                ctx.submit_market(bar.symbol, "buy", self.params["qty"])

Units at the edge. Strategies see real values (float64 prices and quantities, int ns timestamps).
Orders take real values too (str, Decimal, int or float); they are converted to whole ticks and
lots *exactly*. A price that is not a whole number of ticks is rejected `OFF_TICK`, a quantity that
is not a whole number of lots `OFF_LOT` (contracts INS-3), without reaching the engine. Inside the
engine everything is integer (NUM-1).

Look-ahead. `ctx.history()` only holds what was already delivered to the strategy, so it can never
contain a bar whose close time is after `ctx.now` (TIME-3). The arrays are read-only views. This
guards against honest mistakes; it is not a sandbox (PLAN §4.3).
"""

import weakref
from collections.abc import Iterable
from dataclasses import dataclass
from datetime import UTC, datetime
from decimal import Decimal, InvalidOperation
from typing import Any, ClassVar

import numpy as np

from finforge.data.fixedpoint import Grid
from finforge.data.registry import Instrument, Registry

MONEY_SCALE = 100_000_000  # Money units per unit of quote currency (contracts §1)
SIDES = ("buy", "sell")
TIFS = ("gtc", "ioc", "fok")
BAR_FIELDS = ("ts", "ts_open", "open", "high", "low", "close", "volume")
QUOTE_FIELDS = ("ts", "bid", "ask", "bid_qty", "ask_qty")


# --- Exact conversion ----------------------------------------------------------------------------


def to_decimal(value: Any) -> Decimal:
    """Exact decimal of a user value. Floats go through their shortest round-trip text, so 0.1 is
    0.1, not 0.1000000000000000055…"""
    if isinstance(value, bool):
        raise TypeError("a bool is not a number")
    if isinstance(value, Decimal):
        d = value
    elif isinstance(value, int):
        d = Decimal(value)
    elif isinstance(value, float):
        d = Decimal(repr(value))
    elif isinstance(value, str):
        try:
            d = Decimal(value.strip())
        except InvalidOperation:
            raise ValueError(f"not a number: {value!r}") from None
    else:
        raise TypeError(f"expected a number or a decimal string, got {type(value).__name__}")
    if not d.is_finite():
        raise ValueError(f"not a finite number: {value!r}")
    return d


def to_grid(value: Any, grid: Grid) -> int | None:
    """Whole grid steps of `value`, or None when it is not on the grid."""
    scaled = to_decimal(value).scaleb(grid.digits)
    if scaled != scaled.to_integral_value():
        return None
    q, r = divmod(int(scaled), grid.units)
    return q if r == 0 else None


def from_grid(steps: int, grid: Grid) -> float:
    """Float value of whole grid steps (correctly rounded: one exact-integer division)."""
    return steps * grid.units / 10**grid.digits


def money(units: int) -> float:
    return units / MONEY_SCALE


# --- Data handed to strategies -------------------------------------------------------------------


@dataclass(frozen=True, slots=True)
class Bar:
    symbol: str
    ts_open: int  # ns since epoch, UTC
    ts: int  # close time: when the bar became visible (TIME-2)
    open: float
    high: float
    low: float
    close: float
    volume: float


@dataclass(frozen=True, slots=True)
class Quote:
    symbol: str
    ts: int
    bid: float
    bid_qty: float
    ask: float
    ask_qty: float


@dataclass(slots=True)
class OrderHandle:
    """A strategy's view of one order; kept up to date by the SDK."""

    ref: int  # client reference, from 1 (0 for orders rejected by the SDK itself)
    symbol: str
    side: str
    type: str
    tif: str
    qty: float
    price: float | None
    order_id: int | None = None  # engine id; changes when an amend replaces the order
    status: str = "NEW"
    filled: float = 0.0
    remaining: float = 0.0  # open quantity as the engine last reported it (tracks amends)
    reject_reason: str | None = None
    cancel_reason: str | None = None

    @property
    def is_open(self) -> bool:
        return self.status in ("NEW", "ACCEPTED", "PARTIALLY_FILLED")


@dataclass(frozen=True, slots=True)
class OrderUpdate:
    order: OrderHandle
    status: str
    filled: float
    remaining: float
    reject_reason: str | None
    cancel_reason: str | None


@dataclass(frozen=True, slots=True)
class Fill:
    order: OrderHandle
    symbol: str
    side: str
    price: float
    qty: float
    ts: int
    trade_id: int


# --- Strategy ------------------------------------------------------------------------------------


# JSON Schema fragments for strategy parameters (the API validates against them, D8).
QTY_SCHEMA: dict[str, Any] = {
    "anyOf": [
        {"type": "string", "pattern": r"^[0-9]+(\.[0-9]+)?$"},
        {"type": "number", "exclusiveMinimum": 0},
    ],
    "description": "quantity in instrument units (decimal string preferred)",
}


def params_schema(properties: dict[str, Any], required: list[str] | None = None) -> dict[str, Any]:
    return {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "type": "object",
        "properties": properties,
        "required": required or [],
        "additionalProperties": False,
    }


class Strategy:
    """Base class. Override the hooks you need; all default to doing nothing.

    Parameters given in the config arrive as keyword arguments and are kept in `self.params`.
    Subclasses may declare `defaults` (merged under the given params) and validate in `setup`.
    """

    name: ClassVar[str] = ""
    defaults: ClassVar[dict[str, Any]] = {}
    # JSON Schema of the parameters (validated by the API before a run is queued).
    params_schema: ClassVar[dict[str, Any]] = params_schema({})

    def __init__(self, **params: Any) -> None:
        unknown = set(params) - set(self.defaults) if self.defaults else set()
        if unknown:
            raise ValueError(f"{type(self).__name__}: unknown parameters {sorted(unknown)}")
        self.params: dict[str, Any] = {**self.defaults, **params}
        self.setup()

    def setup(self) -> None:
        """Validate `self.params` (raise ValueError) and initialise state."""

    def on_start(self, ctx: "Context") -> None: ...
    def on_bar(self, ctx: "Context", bar: Bar) -> None: ...
    def on_quote(self, ctx: "Context", quote: Quote) -> None: ...
    def on_fill(self, ctx: "Context", fill: Fill) -> None: ...
    def on_order_update(self, ctx: "Context", update: OrderUpdate) -> None: ...
    def on_end(self, ctx: "Context") -> None:
        """After the last event, once open orders were cancelled (END_OF_RUN). Orders sent here
        are not executed; use it for reporting."""


# --- History -------------------------------------------------------------------------------------


class _Series:
    """Append-only columns with amortised growth; views of the filled prefix."""

    def __init__(self, fields: tuple[str, ...]):
        self.fields = fields
        self.n = 0
        self.cols = {
            f: np.empty(256, dtype=np.int64 if f.startswith("ts") else np.float64) for f in fields
        }

    def append(self, values: tuple) -> None:
        if self.n == len(self.cols[self.fields[0]]):
            for f in self.fields:
                grown = np.empty(2 * self.n, dtype=self.cols[f].dtype)
                grown[: self.n] = self.cols[f][: self.n]
                self.cols[f] = grown
        for f, v in zip(self.fields, values, strict=True):
            self.cols[f][self.n] = v
        self.n += 1

    def view(self, f: str, n: int | None) -> np.ndarray:
        start = 0 if n is None else max(0, self.n - n)
        v = self.cols[f][start : self.n].view()
        v.flags.writeable = False
        return v


# --- Context -------------------------------------------------------------------------------------


@dataclass
class _Local:
    """An order the SDK rejected before it reached the engine (OFF_TICK / OFF_LOT)."""

    handle: OrderHandle
    ts: int


class Context:
    """What a strategy may see and do. One per run, handed to every callback."""

    def __init__(self, engine: Any, registry: Registry, universe: Iterable[str], seed: int):
        # Weak: the engine holds the handler (which holds this context), and Python's GC cannot see
        # references kept inside C++ objects, so a strong one here leaked every finished run.
        self._engine = weakref.proxy(engine)
        self._registry = registry
        self._universe = tuple(universe)
        self._inst: dict[str, Instrument] = {s: registry.by_symbol(s) for s in self._universe}
        self._tick = {s: i.tick for s, i in self._inst.items()}
        self._lot = {s: i.lot for s, i in self._inst.items()}
        self._bars = {s: _Series(BAR_FIELDS) for s in self._universe}
        self._quotes = {s: _Series(QUOTE_FIELDS) for s in self._universe}
        self._by_ref: dict[int, OrderHandle] = {}
        self._by_id: dict[int, OrderHandle] = {}
        self._local: list[_Local] = []
        self.rng = np.random.default_rng(seed)  # deterministic per run seed
        self.seed = seed

    # --- clock and universe ---

    @property
    def now(self) -> int:
        """Simulation time, ns since the Unix epoch (UTC). Never wall-clock time (TIME-5)."""
        return self._engine.now()

    @property
    def time(self) -> datetime:
        return datetime.fromtimestamp(self.now / 1e9, tz=UTC)

    @property
    def symbols(self) -> tuple[str, ...]:
        return self._universe

    def instrument(self, symbol: str) -> Instrument:
        return self._instrument(symbol)

    def _instrument(self, symbol: str) -> Instrument:
        try:
            return self._inst[symbol]
        except KeyError:
            raise KeyError(
                f"{symbol!r} is not in the run's universe {list(self._universe)}"
            ) from None

    # --- orders ---

    def submit_market(self, symbol: str, side: str, qty: Any) -> OrderHandle:
        """Market order: fills at the next eligible bar's open / the next quote (contracts §7)."""
        return self._submit(symbol, side, "market", "gtc", qty, None)

    def submit_limit(
        self, symbol: str, side: str, qty: Any, price: Any, tif: str = "gtc"
    ) -> OrderHandle:
        return self._submit(symbol, side, "limit", tif, qty, price)

    def _submit(
        self, symbol: str, side: str, type_: str, tif: str, qty: Any, price: Any
    ) -> OrderHandle:
        inst = self._instrument(symbol)
        side, tif = side.lower(), tif.lower()
        if side not in SIDES:
            raise ValueError(f"side must be one of {SIDES}, not {side!r}")
        if tif not in TIFS:
            raise ValueError(f"tif must be one of {TIFS}, not {tif!r}")
        lots = to_grid(qty, self._lot[symbol])
        ticks = 0 if price is None else to_grid(price, self._tick[symbol])
        handle = OrderHandle(
            ref=0,
            symbol=symbol,
            side=side,
            type=type_,
            tif=tif,
            qty=float(to_decimal(qty)),
            price=None if price is None else float(to_decimal(price)),
        )
        handle.remaining = handle.qty
        reason = "OFF_TICK" if ticks is None else "OFF_LOT" if lots is None else None
        if reason is not None:  # INS-3: rejected at conversion, never reaches the engine
            handle.status, handle.reject_reason = "REJECTED", reason
            self._local.append(_Local(handle, self.now))
            return handle
        ref = self._engine.submit(inst.instrument_id, side, type_, tif, ticks, lots)
        handle.ref = ref
        self._by_ref[ref] = handle
        return handle

    def cancel(self, order: OrderHandle) -> None:
        """Cancel an open order (the engine refuses unknown or terminal orders)."""
        if order.order_id is not None:
            self._engine.cancel(order.order_id)

    def amend(self, order: OrderHandle, qty: Any, price: Any) -> None:
        """ORD-7: new total quantity and price. A price change or a larger quantity replaces the
        order (new engine id, same handle)."""
        if order.order_id is None or order.type != "limit":
            raise ValueError("only accepted limit orders can be amended")
        lots = to_grid(qty, self._lot[order.symbol])
        ticks = to_grid(price, self._tick[order.symbol])
        if lots is None or ticks is None:
            raise ValueError("amend price/qty must be on the instrument's tick/lot grid")
        self._engine.amend(order.order_id, lots, ticks)

    def open_orders(self, symbol: str | None = None) -> list[OrderHandle]:
        return [
            h for h in self._by_ref.values() if h.is_open and (symbol is None or h.symbol == symbol)
        ]

    def target_position(self, symbol: str, target: Any) -> OrderHandle | None:
        """Market order for the difference between `target` and position + open orders."""
        lot = self._lot[symbol]
        want = to_grid(target, lot)
        if want is None:
            raise ValueError(f"target {target!r} is not a whole number of lots")
        have = self.position_lots(symbol)
        for h in self.open_orders(symbol):
            rest = to_grid(h.remaining, lot)  # not qty - filled: an amend replaces both
            have += rest if h.side == "buy" else -rest
        diff = want - have
        if diff == 0:
            return None
        qty = Decimal(abs(diff) * lot.units).scaleb(-lot.digits)  # exact, on the lot grid
        return self._submit(symbol, "buy" if diff > 0 else "sell", "market", "gtc", qty, None)

    def round_price(self, symbol: str, price: Any, mode: str = "nearest") -> Decimal:
        """`price` snapped to the tick grid ("nearest", "down" or "up"), as an exact Decimal."""
        tick = self._tick[symbol]
        steps = to_decimal(price).scaleb(tick.digits) / tick.units
        rounding = {"nearest": "ROUND_HALF_EVEN", "down": "ROUND_FLOOR", "up": "ROUND_CEILING"}[
            mode
        ]
        whole = int(steps.to_integral_value(rounding=rounding))
        return Decimal(whole * tick.units).scaleb(-tick.digits)

    # --- account ---

    def position_lots(self, symbol: str) -> int:
        return self._engine.position(self._instrument(symbol).instrument_id)[0]

    def position(self, symbol: str) -> float:
        """Signed position in instrument units (e.g. BTC)."""
        return from_grid(self.position_lots(symbol), self._lot[symbol])

    @property
    def cash(self) -> float:
        return money(self._engine.account()["cash"])

    @property
    def equity(self) -> float:
        """Cash + Σ position x mark (ACC-6)."""
        return money(self._engine.account()["equity"])

    def account(self) -> dict[str, float]:
        return {k: money(v) for k, v in self._engine.account().items()}

    # --- history ---

    def history(
        self, symbol: str, field: str, n: int | None = None, kind: str = "bars"
    ) -> np.ndarray:
        """The last `n` values (all if None) of `field` for `symbol`, oldest first, from bars (or
        quotes) already delivered. Read-only; never later than `now` (TIME-3)."""
        self._instrument(symbol)
        series = {"bars": self._bars, "quotes": self._quotes}.get(kind)
        if series is None:
            raise ValueError("kind must be 'bars' or 'quotes'")
        s = series[symbol]
        if field not in s.fields:
            raise ValueError(f"{kind} fields are {s.fields}, not {field!r}")
        if n is not None and n < 0:
            raise ValueError("n must be >= 0")
        return s.view(field, n)

    def bars_seen(self, symbol: str) -> int:
        return self._bars[symbol].n


class _Runner:
    """The handler the C++ engine calls, with integers; turns them into SDK objects."""

    def __init__(self, strategy: Strategy, ctx: Context, registry: Registry):
        self.strategy = strategy
        self.ctx = ctx
        self.symbol = [i.symbol for i in registry]
        self.tick = [i.tick for i in registry]
        self.lot = [i.lot for i in registry]

    def on_start(self) -> None:
        self.strategy.on_start(self.ctx)

    def on_bar(
        self, inst: int, ts_open: int, ts_close: int, o: int, h: int, lo: int, c: int, v: int
    ) -> None:
        sym, tick, lot = self.symbol[inst], self.tick[inst], self.lot[inst]
        bar = Bar(
            sym,
            ts_open,
            ts_close,
            from_grid(o, tick),
            from_grid(h, tick),
            from_grid(lo, tick),
            from_grid(c, tick),
            from_grid(v, lot),
        )
        # Recorded before the strategy sees it: history ends with the bar just delivered.
        self.ctx._bars[sym].append(
            (ts_close, ts_open, bar.open, bar.high, bar.low, bar.close, bar.volume)
        )
        self.strategy.on_bar(self.ctx, bar)

    def on_quote(self, inst: int, ts: int, bid: int, bid_qty: int, ask: int, ask_qty: int) -> None:
        sym, tick, lot = self.symbol[inst], self.tick[inst], self.lot[inst]
        q = Quote(
            sym,
            ts,
            from_grid(bid, tick),
            from_grid(bid_qty, lot),
            from_grid(ask, tick),
            from_grid(ask_qty, lot),
        )
        self.ctx._quotes[sym].append((ts, q.bid, q.ask, q.bid_qty, q.ask_qty))
        self.strategy.on_quote(self.ctx, q)

    def on_order_update(
        self,
        ref: int,
        order_id: int,
        status: str,
        filled: int,
        remaining: int,
        reject: str,
        cancel: str,
    ) -> None:
        h = self.ctx._by_ref[ref]
        lot = self.ctx._lot[h.symbol]
        h.order_id = order_id
        self.ctx._by_id[order_id] = h
        h.status = status
        h.filled = from_grid(filled, lot)
        h.remaining = from_grid(remaining, lot)
        h.reject_reason = None if reject == "NONE" else reject
        h.cancel_reason = None if cancel == "NONE" else cancel
        self.strategy.on_order_update(
            self.ctx,
            OrderUpdate(h, status, h.filled, h.remaining, h.reject_reason, h.cancel_reason),
        )

    def on_order_amended(self, order_id: int, qty: int, remaining: int) -> None:
        h = self.ctx._by_id[order_id]
        lot = self.ctx._lot[h.symbol]
        h.qty, h.remaining = from_grid(qty, lot), from_grid(remaining, lot)

    def on_fill(
        self, order_id: int, trade_id: int, inst: int, side: str, price: int, qty: int, ts: int
    ) -> None:
        h = self.ctx._by_id[order_id]
        fill = Fill(
            h,
            self.symbol[inst],
            side.lower(),
            from_grid(price, self.tick[inst]),
            from_grid(qty, self.lot[inst]),
            ts,
            trade_id,
        )
        self.strategy.on_fill(self.ctx, fill)


__all__ = [
    "Bar",
    "Context",
    "Fill",
    "OrderHandle",
    "OrderUpdate",
    "Quote",
    "Strategy",
    "from_grid",
    "to_decimal",
    "to_grid",
]
