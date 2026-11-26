# Analytics, comparison and sweeps

What a run's `report.json` contains, how every number is defined, how runs are compared, and how
parameter sweeps run (PLAN.md Phase 6). The conventions are contract rules MET-1…10 in
[contracts.md](contracts.md) §11. The code is in `python/finforge/analytics/`.

## 1. What a completed run produces

`finforge backtest` (or `finforge.backtest.run`) writes the Phase 5 artefacts. A **completed** run
also gets:

| File | Content |
|---|---|
| `report.json` | performance, trades, exposure, per-instrument P&L, benchmark, conventions |
| `trades.parquet` | FIFO round trips (§3) |
| `exposure.parquet` | per equity point and instrument: position (lots) and mark (ticks) |
| `benchmark/` | a complete buy-and-hold run of the same universe (§5) |

Failed runs keep their partial artefacts but get no report. `finforge report RUN_DIR` rebuilds the
report from the artefacts alone.

## 2. Performance metrics

| Field | Definition |
|---|---|
| returns | simple returns of equity sampled at each UTC day's close; a point exactly at midnight closes the day that ended; days without points carry the last value forward; the first return is against the initial capital (MET-2) |
| `total_return` | final ÷ initial − 1 |
| `cagr` | (final ÷ initial)^(N ÷ periods) − 1 |
| `volatility` | std(r, ddof=1) × √N (MET-6) |
| `sharpe` | mean(excess) ÷ std(excess, ddof=1) × √N; the risk-free rate is converted per period as (1+rf)^(1/N) − 1 (MET-3, MET-4) |
| `sortino` | mean(excess) ÷ √mean(min(r,0)²) × √N over all periods; with no downside and a positive mean it is `null` with `sortino_infinite: true` (MET-5) |
| `max_drawdown`, `drawdown` | largest peak-to-trough fall of the **full** equity curve (not daily), with the initial capital as the first point; `drawdown` gives the peak, trough and recovery times, the episode's duration (to recovery, or to the end) and the longest time under water (MET-7) |
| `var`, `cvar` | historical 95 %, one period, positive loss fractions; the quantile uses linear interpolation (numpy's default, as in empyrical); CVaR is the mean of the returns at or below it (MET-8) |
| `calmar` | cagr ÷ max drawdown |
| `undefined` | every metric reported as `null` (for example Sharpe with fewer than 2 returns or zero volatility). JSON has no NaN or infinity, so a null is never silently a number |

N = `periods_per_year` (default 365; 252 for equities). The conventions used are written into
every report.

## 3. Trades

A **trade** is one FIFO match between an opening lot and closing quantity of the same instrument
(MET-1). A fill bigger than the open position closes it and opens the remainder the other way (a
flip).

- **Gross P&L** is exact integer Money.
- **Fees:** each fill's fee is shared among its pieces pro rata by quantity (a float share of the
  exact Money fee). The trades' fees therefore add up to the fees of the fills they consumed, up to
  float rounding. They do not add up to the run's total fees when lots are still open at the end:
  the opening fees of those lots belong to no trade yet (`open_lots`).
- **Execution cost** = fees + the adverse difference between each fill's price and its **reference
  price**:
  - Candle model: the reference is the fill bar's open, before slippage.
  - Quote model: the reference is the quote mid.
  - Limit fills better than the reference count negative (price improvement).

The summary reports:

- count, wins and losses (net of fees), win rate;
- profit factor (`null` with `profit_factor_infinite: true` when nothing lost; MET-9);
- gross profit and gross loss, net P&L, fees, price cost, execution cost;
- the holding-period distribution (min, quartiles, max, mean);
- open lots at the end.

## 4. Exposure and P&L per instrument

At every equity point, from each instrument's position and mark:

| Measure | Definition |
|---|---|
| gross | Σ\|value\| |
| net | Σ value |
| concentration | max share \|value_i\| ÷ gross, and the Herfindahl index Σ share² |
| capital utilisation | gross ÷ equity |

The report summarises time in the market, mean and max gross, utilisation, net fraction and
concentration.

**P&L per instrument** is computed from cash flows, exactly in Money: −Σ signed notional − fees +
final market value. The report checks the identity
`Σ instrument P&L − borrow = final equity − initial capital` exactly and records it as
`pnl_identity_holds`.

## 5. Benchmark

Every completed run is compared with an equal-weight buy-and-hold of its universe (MET-10). The
benchmark is a real second engine run of the built-in `buy_and_hold` strategy, with the same data,
window, capital, fees, slippage and execution model.

- **Sizing:** capital is split equally; each instrument is bought on its first bar (or quote) for
  whole lots worth `share × (1 − 2 %)` at that close.
- **Overflow:** whatever still does not fit at the next open is cut at fill time (OP-1).

The report carries the benchmark's performance and `excess_total_return`. Set `benchmark: false` in
the config to skip it; sweeps skip it by default.

## 6. Comparing runs

```bash
finforge compare runs/a runs/b runs/c --out runs/compare
```

This prints a metrics table (return, CAGR, volatility, Sharpe, Sortino, max drawdown, VaR, trades,
win rate, profit factor, fees). `--out` also writes `compare.json` and `series.parquet`, which holds
every run's equity, as growth of 1, and its drawdown, aligned on UTC daily closes. Runs with
different windows are forward-filled, and read 1.0 before they start.

## 7. Parameter sweeps (in-sample / out-of-sample)

```yaml
# examples/ma_cross_sweep.yaml
base: examples/ma_cross.yaml
grid: {fast: [10, 20, 30, 45, 60], slow: [180, 360]}
holdout: 2024-01-20T00:00:00Z
processes: 4
```

```bash
finforge sweep examples/ma_cross_sweep.yaml
```

- **Runs:** every grid point runs twice, in-sample (base start to holdout) and out-of-sample
  (holdout to base end), as independent backtests on a process pool. The pool uses the spawn start
  method with one engine per process; sweeps never use threads (PLAN §4.3).
- **Output:** `results.parquet` and `results.json` hold one row per point with `param_*`, `is_*`
  and `oos_*` columns, and every run directory is kept.
- **Cold start:** the out-of-sample run starts cold, so indicators warm up again after the holdout.

On the real BTCUSDT January 2024 data, the 10-point grid above ran 20 backtests in 5.3 s of wall
time (19 s of CPU) on 4 processes.

| fast | slow | IS return | IS Sharpe | IS max DD | OOS return | OOS Sharpe | OOS max DD |
|---|---|---|---|---|---|---|---|
| 10 | 180 | −17.48 % | −15.88 | 19.43 % | −9.86 % | −17.63 | 10.42 % |
| 60 | 360 | −5.12 % | −4.17 | 8.56 % | −1.47 % | −3.67 | 2.78 % |

(2 of the 10 rows.) Ranking configurations in-sample and checking the ranking out of sample is the
point of the split. Picking the best in-sample row and reporting its in-sample numbers is
overfitting.

## 8. Validation

- **Hand-computed fixtures:** `test_metrics.py` checks Sharpe, Sortino, volatility, CAGR, Calmar,
  max drawdown (peak, trough and duration), VaR and CVaR, the risk-free conversion and the daily
  resampling against values derived by hand in the comments, to **1e-9**. It also covers undefined
  and infinite cases.
- **Independent library:** the same metrics are cross-checked against **empyrical-reloaded**, to
  1e-9, on 500 random daily returns. empyrical is a dev dependency only.
- **Trades and exposure:** `test_analytics.py` checks FIFO trades (including partial matches, a
  flip, a short and pro-rata fees, all by hand), exposure and per-instrument P&L by hand, the report
  and benchmark of an end-to-end run, compare, and a 10-config sweep with 4 processes.
