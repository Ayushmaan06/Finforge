# Tutorial: your first strategy

From an empty directory to a tested, compared and swept strategy in about ten minutes, on synthetic
data (no download). The reference for everything used here is [`sdk.md`](sdk.md), and the meaning
of each metric is in [`analytics.md`](analytics.md).

You need the Python package installed (README, "Quickstart"), with its virtual environment active
so that `finforge` is on your `PATH`. Work in any directory; all paths below are relative to it.

## 1. Get a dataset

```bash
python -c "from finforge.data.demo import make_demo_dataset; make_demo_dataset('data/demo', days=90)"
finforge data info data/demo
```

```text
demo-90d-1m  id ce722770780d15c8c18f194c76de4a31c40a4b2953666e8ef1d28773266cd311
time range (ns): {'first': 1704067200000000000, 'last_emit': 1711843200000000000}
  bars   DEMO         rows     129600  bars/DEMO.parquet
validation: clean
```

90 days of 1-minute bars for one instrument, `DEMO`, starting 2024-01-01. The prices are a
seeded random walk with volatility regimes, so the dataset is identical on every machine.
- The **id** is a hash of the content. A run records it, so you always know exactly which data
  produced a result.
- **Real data** works the same way once ingested: `python scripts/fetch_binance.py --bars` and
  `finforge data ingest` (see [`data.md`](data.md)).

## 2. Write the strategy

`breakout.py`:

```python
"""Breakout: buy a new n-bar high, exit below the n-bar low."""

from finforge.sdk import Strategy


class Breakout(Strategy):
    name = "breakout"
    defaults = {"n": 60, "qty": "1"}

    def setup(self):
        self.n = int(self.params["n"])
        if self.n < 2:
            raise ValueError("n must be >= 2")

    def on_bar(self, ctx, bar):
        # The last n+1 closed bars, oldest first, ending with this one. Never later than now.
        high = ctx.history(bar.symbol, "high", self.n + 1)
        low = ctx.history(bar.symbol, "low", self.n + 1)
        if len(high) <= self.n:
            return  # not enough history yet
        flat = ctx.position(bar.symbol) == 0
        if flat and bar.close > high[:-1].max():
            ctx.submit_market(bar.symbol, "buy", self.params["qty"])
        elif not flat and bar.close < low[:-1].min():
            ctx.target_position(bar.symbol, 0)
```

How this code is called:
- **`on_bar` runs when a bar closes.** `bar` is the bar that just closed, and
  `ctx.history(...)[-1]` is that same bar. `high[:-1]` is the n bars *before* it.
- **An order sent now fills at the next bar's open, at the earliest** (Candle model, CND-1). A
  strategy can never trade at a price it has just seen: look-ahead is impossible by construction.
- **Quantities are real numbers.** They are converted to whole lots *exactly*. `"1"` is one unit;
  a quantity off the lot grid would be rejected `OFF_LOT`, not rounded.
- **`target_position(symbol, 0)`** sends one market order for whatever brings the position, plus
  any open orders, to zero.

## 3. Configure and run

`breakout.yaml`:

```yaml
dataset: data/demo
universe: [DEMO]
start: 2024-01-01T00:00:00Z
end: 2024-03-31T00:00:00Z
initial_capital: 10000
execution:
  model: candle
  slippage: {kind: fixed_bps, bps: 1}
fees: {maker_bps: 2, taker_bps: 10}
strategy:
  name: breakout.py:Breakout
  params: {n: 60, qty: "1"}
output_dir: runs
```

```bash
finforge backtest breakout.yaml --run-dir runs/n60
```

```text
status       completed
run dir      runs/n60
strategy     breakout.py:Breakout {"n": 60, "qty": "1"}
dataset      demo-90d-1m (ce722770780d)
events       130315  orders 715  fills 715
equity       10000.00 -> 9999.12 (-0.0088%), fees 35.14
reconciles   True
journal      53333000ef8856849c9991fb9b50dec7f173da3b165c1a4b6d4c0def09bb399c
wall time    1.928 s
```

- **`reconciles True`:** the ledger's cash, positions and realised P&L were rebuilt from the fills
  and matched to the last unit (10⁻⁸). A run that did not reconcile would abort.
- **`journal`** is the run's fingerprint, the SHA-256 of every event. Run the same command again
  and you get the same hash.
- **Exit codes:** 0 ok, 1 config or data error, 3 the strategy raised. On a failure, the traceback
  is in the manifest and the partial artefacts are kept.

## 4. Read the results

```bash
finforge report runs/n60
ls runs/n60
```

```text
return -0.0088%  sharpe -0.09507475191385287  max dd 0.5261%  trades 357
benchmark  equity.parquet  exposure.parquet  fills.parquet  manifest.json  orders.parquet
report.json  trades.parquet
```

| File | What it holds |
|---|---|
| `manifest.json` | the config, dataset id, strategy hash, engine version, journal hash, counts and status: enough to reproduce the run |
| `orders.parquet`, `fills.parquet` | every order and fill, in integer ticks and lots plus real values |
| `equity.parquet`, `exposure.parquet` | equity and positions at every bar |
| `trades.parquet` | FIFO round trips |
| `report.json` | every metric, its conventions (365-day year, risk-free 0, historical VaR …) and the benchmark |
| `benchmark/` | the same config run with buy-and-hold, for comparison |

From Python:

```python
import json

r = json.load(open("runs/n60/report.json"))
r["trades"]["count"], r["trades"]["win_rate"], r["trades"]["profit_factor"]
# (357, 0.325, 0.983)
r["benchmark"]["performance"]["total_return"]  # buy-and-hold made +11.9 % on the same data
```

Two things to notice:

- **The trades' fees are lower than the run's total fees** (35.03 vs 35.14). A *trade* is a closed
  round trip. The position still open at the end paid its opening fee, but it is not a trade yet.
- **Execution cost (42.36) is larger than the fees.** It adds the 1 bp slippage you configured,
  measured against each fill's reference price (here, the bar's open).

## 5. Compare two parameter sets

```bash
sed 's/n: 60/n: 240/' breakout.yaml > breakout_240.yaml
finforge backtest breakout_240.yaml --run-dir runs/n240
finforge compare runs/n60 runs/n240
```

```text
run                        return    cagr    vol  sharpe  sortino  max dd  VaR95  trades  win    PF   fees
breakout.py:Breakout:n60   -0.01%  -0.04%  0.37%   -0.10    -0.13   0.53%  0.03%     357  32%  0.98  35.14
breakout.py:Breakout:n240  +0.39%  +1.58%  0.42%    3.72     6.69   0.31%  0.02%     104  40%  1.54   9.78
```

The slower breakout trades a third as often, so it pays a quarter of the fees. That is often the
whole story with fast strategies.

## 6. Sweep, with an out-of-sample check

A better number on the data you tuned on proves little. The sweep runs every grid point twice:
once *in-sample* (before the holdout date) and once *out-of-sample* (after it), with indicators
warming up again.

`breakout_sweep.yaml`:

```yaml
base: breakout.yaml
grid:
  n: [30, 60, 120, 240, 480]
holdout: 2024-03-01T00:00:00Z
processes: 4
output_dir: runs/sweeps
```

```bash
finforge sweep breakout_sweep.yaml --out runs/sweeps/breakout
```

```text
  n  is_total_return  is_sharpe  is_max_drawdown  oos_total_return  oos_sharpe  oos_max_drawdown
 30           -0.79%     -15.23            0.79%            +0.28%       10.42             0.07%
 60           -0.49%      -9.95            0.53%            +0.49%       20.33             0.03%
120           -0.20%      -4.48            0.23%            +0.58%       20.37             0.04%
240           -0.28%      -5.06            0.31%            +0.67%       21.77             0.04%
480           -0.40%      -6.87            0.42%            +0.73%       21.26             0.06%
```

Every setting lost in-sample and won out-of-sample. The parameter did not matter much; the
**period** did. March happened to trend, while January–February did not. This is the lesson the
split exists to teach: before believing a parameter choice, check that its ranking survives the
holdout, and look at how the market itself behaved.

## 7. Reproduce it

```bash
finforge backtest breakout.yaml --run-dir runs/n60-again | grep journal
```

```text
journal      53333000ef8856849c9991fb9b50dec7f173da3b165c1a4b6d4c0def09bb399c
```

It is the same hash as in step 3, and the same Parquet bytes. `manifest.json` records everything
needed to repeat a run anywhere.

## Next steps

- **More data and costs:** run the strategy on real Binance bars, or on quotes with the Quote
  model (`execution: {model: quote}`). See [`execution.md`](execution.md) for exactly how each
  model fills.
- **More instruments:** add symbols to `universe`. `on_bar` is called per instrument in a
  deterministic order. `examples/pairs.yaml` trades a spread between two.
- **Speed:** on short windows, numpy's `.mean()` is mostly call overhead, and `.sum() / n` is
  bit-identical and 2.5× faster. Profile with `py-spy record --native -- finforge backtest …`
  ([`benchmarks.md`](benchmarks.md)).
- **In the browser:** `docker compose up --build`, then open http://localhost:3000. The dashboard
  runs the built-in strategies; custom strategy files like this one run locally through the SDK
  (the server never executes uploaded code).
