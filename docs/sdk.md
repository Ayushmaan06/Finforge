# Python strategy SDK

This page covers writing a strategy in Python, running it on a dataset, and reading what the run
produces (PLAN.md Phase 5). New to FinForge? Start with the step-by-step
[tutorial](tutorial.md); this page is the reference. The engine underneath is the one described in
[execution.md](execution.md), and the data comes from [data.md](data.md).

## 1. Quick start

```bash
python scripts/fetch_binance.py --bars
finforge data ingest datasets/specs/binance-spot-1m.json --out datasets/normalized/binance-spot-1m
finforge backtest examples/ma_cross.yaml
```

```text
status       completed
run dir      runs/20261125T093700Z-ma_cross-1a2b3c4d
events       44944  orders 304  fills 304
equity       10000.00 -> 8755.43 (-12.4457%), fees 1304.97
reconciles   True
journal      6208230033004496ececc80f0eefbcfa0ae7029401ca46c228f0c03c73b2629a
```

From Python:

```python
from finforge.backtest import run

result = run("examples/ma_cross.yaml")
result.status, result.journal_hash, result.reconciles
fills = result.read("fills").to_pandas()  # if pandas is installed
```

The CLI exits with 0 on success, 1 for a configuration or data error, and 3 when the strategy
failed.

## 2. Writing a strategy

```python
from finforge.sdk import Strategy


class Breakout(Strategy):
    """Buy a new 20-bar high, exit below the 20-bar low."""

    name = "breakout"
    defaults = {"n": 20, "qty": "0.01"}

    def setup(self):  # validate self.params
        if int(self.params["n"]) < 2:
            raise ValueError("n must be >= 2")

    def on_bar(self, ctx, bar):
        n = int(self.params["n"])
        high = ctx.history(bar.symbol, "high", n + 1)
        low = ctx.history(bar.symbol, "low", n + 1)
        if len(high) <= n:
            return
        if ctx.position(bar.symbol) == 0 and bar.close > high[:-1].max():
            ctx.submit_market(bar.symbol, "buy", self.params["qty"])
        elif ctx.position(bar.symbol) > 0 and bar.close < low[:-1].min():
            ctx.target_position(bar.symbol, 0)
```

Reference it from a config with `strategy: {name: "path/to/breakout.py:Breakout"}` or
`"my_package.module:Breakout"`. The built-in strategies are listed in §5.

### Hooks

| Hook | When |
|---|---|
| `on_start(ctx)` | before the first event |
| `on_bar(ctx, bar)` | at each bar's close time (TIME-2); `bar.ts` is the close and `bar.ts_open` the open |
| `on_quote(ctx, quote)` | at each quote's exchange time (QuoteModel runs) |
| `on_order_update(ctx, update)` | every status change of the strategy's own orders |
| `on_fill(ctx, fill)` | every fill of the strategy's own orders |
| `on_end(ctx)` | after the last event, once open orders are cancelled (`END_OF_RUN`). For reporting only: orders sent here do not execute |

### Context

| Member | |
|---|---|
| `now`, `time` | simulation time in ns since the epoch (UTC), or as a `datetime`. Never wall-clock time |
| `symbols`, `instrument(s)` | the run's universe and its registry entries (tick, lot, …) |
| `submit_market(s, side, qty)` | fills at the next bar's open, or the next quote ([execution.md](execution.md) §3) |
| `submit_limit(s, side, qty, price, tif="gtc")` | `tif` is one of `gtc`, `ioc`, `fok` |
| `cancel(order)`, `amend(order, qty, price)` | ORD-7: `qty` is the new total quantity |
| `target_position(s, qty)` | one market order for the difference from position + open orders |
| `open_orders(s=None)` | live `OrderHandle`s |
| `position(s)`, `position_lots(s)` | signed, in instrument units or in lots |
| `cash`, `equity`, `account()` | the account in the quote currency (ACC-6) |
| `history(s, field, n=None, kind="bars")` | the last `n` delivered values; bar fields `ts, ts_open, open, high, low, close, volume`; quote fields `ts, bid, ask, bid_qty, ask_qty` |
| `bars_seen(s)` | how many bars of `s` have been delivered |
| `round_price(s, price, mode)` | snaps to the tick grid (`nearest`, `down` or `up`); returns an exact `Decimal` |
| `rng` | a `numpy.random.Generator` seeded with the config's `seed` |

### Units and exactness

Strategies read **real values**: prices and quantities are float64, timestamps are int ns.

Orders take real values too, as `str`, `Decimal`, `int` or `float`. A float is first converted to
its shortest text, so `0.1` stays 0.1. The value is then converted to whole ticks and lots
**exactly**:

- An off-grid price is **rejected `OFF_TICK`**, and an off-grid quantity **`OFF_LOT`** (INS-3).
- The rejection happens in the SDK; the engine never sees the order.
- The returned `OrderHandle` says so: `status="REJECTED"`, `order_id=None`.
- These orders appear in `orders.parquet` with `order_id` 0.

Use `ctx.round_price` to snap a computed price to the grid. Inside the engine everything is an
integer (NUM-1).

### No look-ahead

`ctx.history()` holds only what has already been delivered to the strategy. A bar is delivered at
its close, so history can never contain data later than `ctx.now` (TIME-3). The arrays are
read-only views.

- **Tests:** a strategy asserts the bound on every call, on bars across two instruments and on
  quotes (`test_backtest.py`).
- **Fill timing:** orders fill only on later bars. The engine-level look-ahead tests are in
  [execution.md](execution.md) §3.
- **Limits:** this guards against honest mistakes. It is not a sandbox (PLAN §4.3).

## 3. Configuration

YAML, validated by `finforge.config.BacktestConfig` (pydantic). Unknown keys are errors. Decimal
values are converted to engine units **exactly**; a value finer than the engine's resolution is an
error, never rounded.

```yaml
dataset: datasets/normalized/binance-spot-1m   # a finforge.data dataset directory
dataset_id: 256e5d6e…                          # optional: must match the manifest
universe: [BTCUSDT]
start: 2024-01-01T00:00:00Z                     # optional; a timezone is required (TIME-1)
end: 2024-02-01T00:00:00Z                       # optional; bars with open >= start, close <= end
initial_capital: 10000                          # quote currency
execution:
  model: candle                                 # candle | quote
  slippage: {kind: fixed_bps, bps: 1}           # none | fixed_bps | half_spread (candle only)
  participation: 0                              # candle volume cap, fraction of bar volume; 0 = off
  latency_ns: 1                                 # >= 1 (EVT-3)
fees: {maker_bps: 2, taker_bps: 10, min_fee: 0}
risk: {allow_short: false, max_position_lots: null, max_leverage: 1, borrow_rate: 0}
seed: 42
strategy: {name: ma_cross, params: {fast: 30, slow: 240, qty: "0.1"}}
equity_interval_s: null                         # null: one equity point per bar
output_dir: runs
```

## 4. Run directory

| File | Content |
|---|---|
| `manifest.json` | see below |
| `orders.parquet` | every order: ids, client ref, symbol, side, type, tif, price and qty (ticks and lots), filled, cancelled, status, reject and cancel reasons, submit and sent times, `price_real`, `qty_real` |
| `fills.parquet` | every fill: ts, order and trade ids, symbol, side, price, qty, fee (Money), liquidity, `*_real` |
| `equity.parquet` | equity curve: ts, cash, equity, realised and unrealised P&L, fees, borrow, gross exposure (Money), `equity_real` |

`manifest.json` holds:

- **status:** `status`, `partial`, and `error` (type, message, traceback).
- **inputs:** the full `config`, `dataset` (id, name, path), `seed`, and `strategy` (name, class,
  source SHA-256, params, and `hash` = SHA-256 of all three; D6).
- **engine:** `engine` (version, git sha, build type) and `execution_model`.
- **results:** `journal_hash` (DET-1), `counts`, `reconciles` (ACC-10 and ACC-11), `summary`
  (equity, return, P&L, fees, borrow), and `artefacts` (SHA-256 of each Parquet file).
- **timings:** `timings` (start time and wall seconds) is the only wall-clock data in a run.

**Determinism.** Running the same config twice gives the same journal hash and byte-identical
Parquet files. This is tested on fixtures and was checked on the real sample dataset.

**Failure.** If the strategy raises, the run stops:

- `status` is `failed`, the traceback is in the manifest, and `partial` is true.
- Everything up to the failure is still written: orders, fills and equity.
- `reconciles` reports whether the ledger was consistent at that point.
- The CLI exits with 3.

## 5. Reference strategies

Each strategy's docstring states its logic and assumptions. The tests run each one on crafted
prices and check that it fires on exactly the bars computed by hand (`test_strategies.py`).

| Name | Logic | Key params |
|---|---|---|
| `ma_cross` | long when SMA(fast) crosses above SMA(slow), flat on the cross back | `fast`, `slow`, `qty` |
| `mean_reversion` | buy when the z-score of the close against its rolling mean is below −entry, exit above −exit (optionally mirrored short) | `lookback`, `entry_z`, `exit_z`, `qty`, `allow_short` |
| `momentum` | every `rebalance` bars, hold `qty` if the return over `lookback` bars is positive, else flat (or short) | `lookback`, `rebalance`, `qty`, `allow_short` |
| `pairs` | trade the spread z-score of `log A − beta·log B`; needs `risk.allow_short` | `a`, `b`, `lookback`, `entry_z`, `exit_z`, `beta`, `qty_a`, `qty_b` |

All four ran on the real sample dataset (`examples/*.yaml`) with fees and slippage, and every run
reconciled:

| Example | Bars | Fills | Wall time |
|---|---|---|---|
| `ma_cross`, BTCUSDT, Jan 2024 | 44 640 | 304 | 1.7 s |
| `mean_reversion`, ETHUSDT, 2024 | 527 040 | 1 445 | 43 s |
| `momentum`, BTC + ETH, 2024 | 1 054 080 | 725 | 7.9 s |
| `pairs`, ETH/BTC, Jan–Feb 2024 | 172 800 | 518 | 6.6 s |

They are references for the SDK, not trading advice; with taker fees at 1-minute frequency they all
lose money.

## 6. How it fits together

```text
finforge.backtest.run(config)
  ├─ Dataset (manifest, registry) ── iter_chunks (zero-copy, optional time window)
  ├─ _engine.Backtest (C++): CandleModel|QuoteModel + Accounts + Journal + Simulator + Replay
  │     Dispatcher (C++ Strategy) ──ints──► _Runner (Python) ──SDK objects──► your Strategy
  │     Context actions ──► Backtest.submit/cancel/amend (only inside a callback)
  └─ results: orders / fills / equity arrays ──► Parquet; manifest.json
```

- **One run per process.** The GIL is held for the whole run.
- **Errors:** C++ exceptions surface as Python exceptions, and a strategy exception surfaces
  unchanged.
- **Ledger bugs:** a ledger inconsistency is an engine bug and aborts the process (FINFORGE_CHECK).
