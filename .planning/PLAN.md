# FinForge — Formal Project Plan

| | |
|---|---|
| **Status** | Approved baseline v1.0 — source of truth for scope, contracts and roadmap |
| **Date** | 2026-11-11 |
| **Supersedes** | `.planning/rough-plan.md` (kept unchanged for history) |
| **Change process** | Edit this file and add a row to §15 Change Log. Never silently diverge from it. |

---

## Contents

1. Review of the rough plan: gaps found and how they are fixed
2. Vision, scope and non-goals
3. Key architectural decisions
4. Architecture
5. Domain contracts (the rules everything must obey)
6. Roadmap: phases 0–9
7. Testing strategy
8. Repository structure
9. Technology stack
10. Non-functional requirements and targets
11. Risks and mitigations
12. Learning journal process (`.planning/phases/`)
13. Definition of Done (v1.0)
14. Open questions (defaults chosen)
15. Change log

---

# 1. Review of the Rough Plan

The rough plan's vision, module list and philosophy are sound and are kept. The gaps below would have caused rework or wrong results once coding started. Each one is fixed in this document.

| # | Gap in rough plan | Why it matters | Fix (section) |
|---|---|---|---|
| G1 | **Unclear who owns the simulation loop.** The plan puts event processing in C++ but `backtesting/` and `portfolio/` in Python. | Without a decision, the C++/Python boundary gets redesigned twice. Fills and accounting split across languages break determinism and add per-event overhead. | C++ owns the loop, execution and ledger. Python owns data prep, strategies, orchestration and analytics. (§3 D1, §4.2) |
| G2 | **No numeric representation.** Prices and money are never specified. | `double` prices used as order-book keys cause wrong matching (`0.1+0.2 != 0.3`). Float cash drifts, so accounts never reconcile exactly. | Integer ticks, lots and fixed-point cash. (§3 D2, §5.1) |
| G3 | **No time model.** There is no timestamp format, no timezone, and nothing says whether a bar's time means its open or its close. | Bar-time semantics are the most common source of look-ahead bias. | int64 ns UTC. A bar becomes visible only at its close time. (§5.2) |
| G4 | **No instrument reference data** (tick size, lot size, currency). | Orders can't be validated, and quantities can't be converted to money. | Instrument registry per dataset. (§5.3) |
| G5 | **Order book role in historical mode is overstated.** Candle and quote data can't drive a real limit order book. | Building book-based historical execution first would stall the project. Using the book on OHLCV data would also give fake realism. | One `ExecutionModel` interface with three implementations: Book (Mode B), Candle and Quote. L2/L3 historical replay is post-v1. (§3 D4, §5.7) |
| G6 | **Fill timing is undefined** for the candle model. | Results can't be trusted or reproduced. | Fill rules written down: next-bar open, limit trade-through, optional volume cap. (§5.7) |
| G7 | **Same-timestamp ordering is "to be defined".** | Non-deterministic results. | Sort key `(ts, class, seq)` plus a latency model. (§5.6) |
| G8 | **No pre-trade risk checks.** | Strategies could spend money they don't have or exceed limits, and accounting goes inconsistent. | Pre-trade checks in the engine, with explicit reject reasons. (§5.8) |
| G9 | **Pairs trading needs shorts, but short selling is deferred.** | One of the reference strategies can't be built. | Simple short-selling convention in v1. Pairs is the optional 4th strategy. (§5.8, Phase 5) |
| G10 | **Backtest config takes a single instrument.** | Pairs and portfolio strategies need several instruments. | The config takes a *universe*, and the engine does a deterministic k-way merge. (§5.6, Phase 3) |
| G11 | **Module 8 (Experiment Management) is missing from the roadmap.** | Reproducibility, one of the stated core goals, never gets built. | Run manifest in Phase 5, comparison and sweeps in Phase 6, registry in Phase 7. |
| G12 | **"Version" is undefined** for strategy, dataset and engine. | "Reproducible" can't be checked. | Content hashes and the git SHA. (§3 D6) |
| G13 | **"Trade" is ambiguous** (a fill or a round trip?). | Win rate, trade count and profit factor come out wrong. | Defined: *fill* = execution; *trade* = round trip via FIFO lot matching. (§5.9) |
| G14 | **Metric conventions are left open** (annualisation, risk-free rate, VaR method). | Metrics can't be compared or checked. | Defaults pinned. (§5.9) |
| G15 | **No buy-and-hold benchmark.** | Without a baseline you can't tell whether a strategy adds anything. | Benchmark run per backtest. (Phase 6) |
| G16 | **CI, Docker and integration tests only arrive in Phase 9.** | Bugs pile up for 8 phases, and Windows/Linux drift goes unnoticed. | CI from Phase 0, Postgres Compose in Phase 7, integration tests in every phase. |
| G17 | **Testing is example-based only.** | Order-book bugs hide in rare interleavings that hand-written examples miss. | Differential testing against a naive reference book, randomized invariant checks and sanitizers. (§7) |
| G18 | **Redis and Celery are in the stack with no requirement for them.** | Extra infrastructure to learn and run, for nothing. | Postgres job queue (`SKIP LOCKED`) plus SSE. Redis/Celery deferred until distributed sweeps need them. (§3 D7) |
| G19 | **Uploading strategy code through the API is implied** ("create a strategy"). | That is remote code execution. | The API exposes built-in strategies with parameters only. Custom strategies run locally through the SDK. (§3 D8) |
| G20 | **No cancel or progress hooks in the engine.** | The backend can't cancel jobs or report progress without them. | The engine API has a progress callback and a cancel flag. (Phase 2) |
| G21 | **No data source is named.** | Phase 3 can't start, and data licensing gets ignored. | Binance public data first. Download scripts, no raw data in git. (§3 D5) |
| G22 | **Seeded randomness isn't portable.** `std::uniform_int_distribution` differs between libstdc++ and MSVC. | The same seed gives different results on different machines. | Own PRNG (PCG/splitmix64) plus own distribution transforms. (Phase 2) |
| G23 | **No dev toolchain decision**, and the dev machine is Windows. | Hours lost to MSVC/pybind11/sanitizer friction, plus drift from Linux CI. | WSL2 Ubuntu as the primary dev environment. (§3 D9) |
| G24 | **No performance targets**, only "benchmark it". | There's no way to tell when optimisation is done. | Indicative targets, revised after the first measurement. (§10) |
| G25 | **Per-run time series are stored as Postgres rows** (orders, trades, snapshots). | Millions of rows per minute-bar run. | Run summaries and metrics in Postgres. Per-run series in Parquet artefacts. (§3 D7) |
| G26 | **No way to resume work, and no learning record.** | Context is lost between sessions. | Phase plan, log and summary docs. (§12) |
| G27 | **No overfitting guard on parameter sweeps.** | Comparing runs on the same period rewards curve-fitting. | Sweeps report in-sample and out-of-sample splits (simple holdout). Walk-forward is post-v1. (Phase 6) |

---

# 2. Vision, Scope and Non-Goals

## 2.1 Vision (unchanged)

A high-performance market simulation and research platform. A modern C++20 engine (order book, matching, event-driven simulation, accounting) is driven by Python strategies, evaluated with rigorous analytics, and exposed through an API and a web dashboard.

Priorities, in order: **correctness → determinism → reproducibility → clarity → performance → features.**

## 2.2 Operating modes

| | Mode A — Historical Backtesting | Mode B — Exchange Simulation |
|---|---|---|
| Input | Historical bars and quotes (L1) | Synthetic order flow, or user-submitted orders |
| Execution | Candle or Quote execution model | Real price-time-priority order book |
| Claims | "Approximate execution under documented assumptions" | "Exact matching under our rules" — *not* a reconstruction of a real exchange |
| v1 surface | Python SDK, CLI, API, dashboard | C++ CLI and Python API (no web UI in v1) |

Both modes share: core types, the event engine, the `ExecutionModel` interface, the ledger and the journal.

## 2.3 In scope for v1

Everything in Definition of Done (§13).

## 2.4 Out of scope for v1 (explicit)

Out of scope: live trading, broker or exchange connectivity, real money, HFT infrastructure, multi-user/auth, distributed matching, GPU, ML models, portfolio optimisation, derivatives pricing, uploading user strategy code via the API, and L2/L3 historical book replay with queue-position estimation.

Also out: native stop and stop-limit orders, margin calls and borrow availability, equities corporate actions and trading calendars, walk-forward optimisation, Redis and Celery.

These are in the **post-v1 backlog** (§6, end), not forgotten.

---

# 3. Key Architectural Decisions

Each decision is final for v1 unless changed through §15. The format is lightweight: decision, then rationale, then consequence.

**D1 — The C++ engine owns the loop, execution and accounting. Python owns everything around it.**
*Rationale:* Fills and ledger updates must happen in the same deterministic loop using integer math. The hot path stays in C++. Python only does the work that is naturally flexible: data wrangling, strategy logic, orchestration and post-hoc analytics.
*Consequence:* Strategies are Python callbacks invoked by the C++ loop (a pybind11 trampoline). Analytics run in Python over arrays the engine exports at the end of a run.

**D2 — Fixed-point everywhere in the engine.**
Price is `int64` ticks. Quantity is `int64` lots. Cash and money are `int64` at a fixed scale of 10⁻⁸ of the quote currency. Intermediate products use `__int128`, or a checked multiply on MSVC.
*Rationale:* Exact matching keys, exact reconciliation, and determinism across platforms.
*Consequence:* Doubles appear only at the edges: ingestion converts in, analytics converts out.

**D3 — Determinism is a tested property, not a hope.**
Every run writes an event journal. Its SHA-256 is the run's fingerprint. Running the same inputs twice must give identical fingerprints, and CI checks this.

**D4 — One `ExecutionModel` interface, three v1 implementations: `BookModel` (Mode B), `CandleModel` and `QuoteModel`.**
Models are never presented as equivalent. Every result records which model produced it.

**D5 — The primary data source is Binance public market data** (`data.binance.vision`), in two datasets:
- **Bars (CandleModel):** spot 1-minute klines for BTCUSDT and ETHUSDT, 2023–2024. Each month is about 2 MB zipped.
- **Quotes (QuoteModel):** USD-M futures `bookTicker` for BTCUSDT, 1–2 days in March 2024 (about 340 MB zipped per day), plus futures 1-minute klines for the same days. Spot `bookTicker` is not published, and futures `bookTicker` only covers 2023-05-16 → 2024-03-30 (verified 2026-11-11). We treat the perpetual as a spot-like instrument and ignore funding; this limitation is documented.

*Rationale:* It's free and well documented, it trades 24/7 so there are no trading calendars, it has no corporate actions, and it provides both bars and quotes, which covers both v1 execution models.
*Consequence:* Annualisation defaults to 365 days. Equities are post-v1. Download scripts live in the repo, raw data does not (check the source's terms before redistributing anything). Synthetic generators cover tests.

**D6 — Versions are content hashes.**
- Dataset version: SHA-256 of the normalised Parquet file(s).
- Strategy version: SHA-256 of the strategy source file, plus its name and parameters.
- Engine version: package version plus git SHA plus build type.

All of these go into every run manifest.

**D7 — Storage split.** Postgres stores metadata: datasets, runs, configs, statuses, metric summaries and the job queue. Parquet stores bulk data: market data and per-run orders, fills, equity and journal. The job queue is a Postgres table polled with `SELECT … FOR UPDATE SKIP LOCKED`. Progress is pushed with SSE. **No Redis or Celery in v1.**
*Add them when:* sweeps need to run across several machines.

**D8 — The API runs only built-in strategies** (by name plus validated parameters). Custom strategies are written and run locally with the Python SDK. This keeps arbitrary code execution off the server.

**D9 — Toolchain.** The primary dev environment is WSL2 Ubuntu 24.04 with GCC 13+ or Clang 17+, CMake ≥ 3.28, Ninja and Python 3.12 managed by `uv`. CI runs on Linux. Native Windows (MSVC) is "best effort", not supported in v1.
*Rationale:* Sanitizers, perf tooling, Docker and CI parity all come for free.

**D10 — Dependencies come in via CMake `FetchContent`** (GoogleTest, Google Benchmark, pybind11). There is no vcpkg or Conan. The Python package is built with `scikit-build-core`.

**D11 — Simplest structure first, then benchmark the alternatives.** The order book starts as `std::map<price, Level>` with a FIFO per level and an `unordered_map<OrderId, locator>`. Faster designs (flat price ladder, intrusive lists, object pools) are Phase 9 experiments, and only kept if the benchmark shows a gain.

---

# 4. Architecture

## 4.1 Data flow

```text
 Raw CSV/Parquet ──► [Python] ingest → validate → normalise → Parquet + manifest (hash)
                                                     │
                                       numpy columns (zero-copy)
                                                     ▼
 ┌──────────────────────────── C++ ENGINE (deterministic) ────────────────────────────┐
 │  Feed (k-way merge) ─► Event Queue (ts,class,seq) ─► Dispatcher ─► Clock           │
 │        │                                   │                                       │
 │        ▼                                   ▼                                       │
 │  Strategy callback ◄── Context ──► Pre-trade checks ─► ExecutionModel              │
 │   (Python via trampoline)                    (Book | Candle | Quote)               │
 │                                                       │ fills                      │
 │                                                       ▼                            │
 │                                   Ledger (cash, positions, P&L, fees) ─► Journal   │
 └────────────────────────────────────────────────────────────────────────────────────┘
                                                     │ arrays at end of run
                                                     ▼
 [Python] run artefacts (Parquet + manifest.json) ─► analytics → report.json
                                                     │
 [FastAPI] jobs (Postgres SKIP LOCKED) · runs registry · SSE progress
                                                     │
 [Next.js] dashboard
```

## 4.2 Ownership

| Concern | Lives in | Language | Reason |
|---|---|---|---|
| Core types, order book, matching | `engine/` | C++ | Hot path, exactness |
| Event queue, clock, dispatch, latency | `engine/` | C++ | Determinism |
| Execution models, fees, slippage | `engine/` | C++ | Same loop as fills |
| Pre-trade checks, ledger, journal | `engine/` | C++ | Integer accounting, traceability |
| Synthetic flow generator, Mode B CLI | `engine/` | C++ | Standalone demo |
| Bindings | `python/bindings/` | C++/pybind11 | Thin; no logic |
| Ingestion, validation, catalog | `finforge.data` | Python (pyarrow) | Ecosystem; Arrow C++ is too heavy |
| Strategy SDK and reference strategies | `finforge.strategy(ies)` | Python | User-facing flexibility |
| Backtest config, run, artefacts, CLI | `finforge.backtest` | Python | Orchestration |
| Metrics, reports, comparison, sweeps | `finforge.analytics` | Python (numpy) | Post-hoc, not hot path |
| API, jobs, persistence | `backend/` | Python (FastAPI) | Standard web stack |
| UI | `frontend/` | TypeScript (Next.js) | Consumes the API only; no simulation logic |

## 4.3 The language boundary

- **Data in.** Normalised columns go to C++ as `py::array_t` with no copy. The engine keeps a reference for the life of the run.
- **Callbacks.** The engine calls `strategy.on_bar(ctx, bar)` with the GIL held. One backtest runs per process. Parallel sweeps use multiple processes, never threads.
- **Look-ahead protection by construction.** `ctx.history(...)` returns read-only views bounded to events at or before *now*. This guards against honest mistakes. It is not a sandbox.
- **Errors.** C++ exceptions map to Python exceptions. A Python exception inside a strategy aborts the run and marks it `failed` with the traceback.
- **Data out.** At the end of a run, the engine hands over orders, fills, equity snapshots and the journal as columnar arrays, which are written to Parquet.

---

# 5. Domain Contracts

These are the starting defaults. In Phase 0 they are copied into `docs/contracts.md`, refined, and treated as the spec that tests are written against.

## 5.1 Numeric types

| Type | Representation | Notes |
|---|---|---|
| `Price` | int64 ticks | `real = ticks × instrument.tick_size` |
| `Qty` | int64 lots | `real = lots × instrument.lot_size`; must be > 0 on orders |
| `Money` | int64 at a scale of 1e-8 quote currency | Range is about ±9.2e10 units, which is enough |
| Notional | `price_ticks × qty_lots × (tick_size × lot_size / 1e-8)` | Computed in 128-bit, then range-checked |
| Rounding | Fees round **up** to the minor unit (conservative). Everything else is exact. | Documented and tested |
| IDs | `OrderId`, `TradeId`, `Seq` are monotonically increasing uint64 per run | Never random |

## 5.2 Time

- `Timestamp` is int64 **nanoseconds since the Unix epoch, UTC**. There are no local times anywhere in the engine.
- Bars are labelled by **open time**, with `close_time = open_time + interval`. A bar event is emitted at `close_time`. A strategy can never see bar *t* before it has closed.
- Quotes and trades are emitted at their exchange timestamp.
- The simulation clock only moves forward. It never reads wall-clock time.

## 5.3 Instruments

Each instrument has: `instrument_id` (dense uint32), `symbol`, `asset_class`, `quote_currency`, `tick_size`, `lot_size`, `min_qty`, and `contract_multiplier` (1 for spot).

The registry is stored in the dataset manifest. Orders whose price isn't on a tick, or whose quantity isn't a whole number of lots, are rejected.

## 5.4 Order model and lifecycle

Order fields: `id`, `owner_id`, `instrument_id`, `side`, `type` {MARKET, LIMIT}, `tif` {GTC, IOC, FOK}, `price` (LIMIT only), `qty`, `remaining`, `submit_ts`, `seq`, `status`.

```text
            ┌──────────► REJECTED (terminal; reason code)
 NEW ───────┤
            └──► ACCEPTED ──► PARTIALLY_FILLED ──► FILLED (terminal)
                    │               │
                    └───────────────┴──► CANCELLED (terminal; by user, IOC remainder, or FOK fail)
```

- Every transition emits an event. There are no other transitions. An invalid transition is a bug and is asserted against.
- Reject reasons (enum): `INVALID_QTY`, `INVALID_PRICE`, `OFF_TICK`, `OFF_LOT`, `UNKNOWN_INSTRUMENT`, `INSUFFICIENT_CASH`, `POSITION_LIMIT`, `LEVERAGE_LIMIT`, `SHORT_NOT_ALLOWED`, `UNKNOWN_ORDER`, `ALREADY_TERMINAL`.
- Amend: reducing quantity keeps queue priority. Changing the price or increasing the quantity is cancel-and-replace, which loses priority.

## 5.5 Matching rules (Mode B / `BookModel`)

1. Price-time priority: better price first, then lower `seq` first.
2. The trade price is the **resting** order's price.
3. A market order matches against available liquidity. Any remainder is **cancelled**, never rested.
4. A marketable limit order matches first. Its remainder rests (GTC), is cancelled (IOC), or the whole order is cancelled before any fill if it can't fill completely (FOK).
5. Fully filled resting orders are removed. The book is never crossed after matching completes.
6. There is no self-trade prevention in v1. `owner_id` is recorded so STP can be added later.
7. Invariants (tested): no overfill; total quantity is conserved (`Σ fills + remaining + cancelled = original`); the book is never crossed at rest; every fill references two valid orders.

## 5.6 Event ordering and latency

- The event queue is ordered by `(ts, class, seq)`. Class priority at the same timestamp:
  `0 = exchange actions due (order arrivals, cancels)` → `1 = market data` → `2 = timers / snapshots`.
- When multiple instruments' data share a timestamp, they are ordered by `instrument_id`, then by source row order. This is a deterministic k-way merge.
- **Latency:** an order a strategy submits while handling an event at `t` arrives at the execution model at `t + latency`. The default is `latency = 1 ns` (the minimum possible), and it is configurable. So an order can never trade against the event that triggered it.

## 5.7 Execution models (Mode A)

**CandleModel** (bars):
- An order submitted at bar *t*'s close is first eligible on bar *t+1*.
- **Market:** fills at `open[t+1]` adjusted by slippage.
- **Limit buy:** fills if `low[t+1] < limit` (strict trade-through, which is conservative). The fill price is `min(open[t+1], limit)`. Limit sell is the mirror image.
- **Optional volume cap:** `fill_qty ≤ participation × volume[t+1]` (off by default). Any remainder stays working for GTC, or is cancelled for IOC/FOK and market orders.
- There are no native stops in v1. Strategies implement stop-loss and take-profit by checking at bar close and sending a market order, which avoids the intra-bar ambiguity problem entirely.

**QuoteModel** (L1 bid/ask):
- A market buy fills at the first ask at or after its arrival time, capped by the ask size. The remainder follows TIF rules.
- A limit buy fills when `ask ≤ limit`, at the ask. The same size cap applies.
- The spread cost is implicit because fills happen at the bid or ask.

**BookModel:** delegates to the order book (§5.5).

**Slippage models (v1):** `none`, `fixed_bps`, `half_spread` (Candle only, using a configured spread in bps). Slippage is always adverse to the order.

**Fee models (v1):** `bps_of_notional` plus an optional `min_fee` per fill. Maker and taker rates are separate (BookModel/QuoteModel). Candle fills count as taker.

## 5.8 Portfolio accounting and pre-trade checks

- **Average-cost method.** Increasing a position updates the average price. Reducing it realises P&L against the average price. Flipping a position is a close followed by an open within one fill.
- **Cash.** Fees are debited at the fill. Short-sale proceeds are credited to cash.
- **Reserved cash.** Open buy limit orders reserve `limit × remaining + estimated fee`, released on fill or cancel.
- **Equity** = `cash + Σ (signed_qty × mark_price × multiplier)`. The mark price is the last close (Candle) or the mid (Quote/Book).
- **Shorts** are allowed when `allow_short = true`. There is no borrow cost by default (configurable bps per year, accrued daily). There are no margin calls in v1. A gross-leverage cap is enforced pre-trade instead.
- **Pre-trade checks, in order:** instrument → tick/lot → quantity > 0 → cash (including reserved) for buys → position limit → `allow_short` → gross leverage ≤ `max_leverage` (default 1.0).
- **Reconciliation invariant**, checked after every fill in debug builds and at the end of every run:
  `equity = initial_cash + realised_pnl + unrealised_pnl − total_fees − borrow_costs`, exact in integer units.
  A second check replays the fill ledger from scratch and must reproduce the final cash and positions.

## 5.9 Metrics conventions

| Item | Convention |
|---|---|
| Fill vs trade | *Fill* is one execution. A *trade* is a round trip, built by FIFO matching of fills per instrument. |
| Returns | Simple returns of the equity curve, resampled to the UTC daily close |
| Annualisation | `periods_per_year` is configurable. Default 365 (crypto); 252 for equities. |
| Risk-free rate | Configurable annual rate, converted per period. Default 0. |
| Sharpe | `mean(excess) / std(excess, ddof=1) × √N` |
| Sortino | `mean(excess) / downside_dev × √N`, where downside_dev = `√(mean(min(r−0,0)²))` over all periods |
| Volatility | `std(r, ddof=1) × √N` |
| Max drawdown | Largest peak-to-trough fall of the equity curve, as a fraction. Duration is also reported. |
| VaR / CVaR | Historical, 1-day, 95%, reported as positive loss fractions |
| Profit factor | `gross profit / gross loss` over trades. Infinite if there are no losses (flagged). |
| Win rate | Winning trades / closed trades (net of fees) |
| Benchmark | Buy-and-hold of the same universe (equal weight), with the same capital, fees and execution model |

---

# 6. Roadmap

**Rules for every phase:**
- It leaves behind working, tested software.
- CI stays green.
- It has a `plan.md`, a `log.md` and a `summary.md` (§12).
- Exit criteria are objective checks, not feelings.

Size is relative effort (S < M < L). It is not a calendar promise.

| Phase | Name | Size | Depends on |
|---|---|---|---|
| 0 | Foundations and Contracts | M | — |
| 1 | Order Book and Matching Engine | L | 0 |
| 2 | Event Engine and Exchange Simulation | M | 1 |
| 3 | Historical Data Pipeline | M | 2 |
| 4 | Execution Models and Portfolio Ledger | L | 2, 3 |
| 5 | Python Strategy SDK and End-to-End Backtest | L | 4 |
| 6 | Analytics and Experiment Comparison | M | 5 |
| 7 | Backend, Persistence and Jobs | L | 6 |
| 8 | Research Dashboard | L | 7 |
| 9 | Performance, Hardening and Release | M | 8 |

---

## Phase 0 — Foundations and Contracts

**Objective.** A working toolchain end to end (C++ → Python → tests → CI), and written contracts before any domain code.

**You will learn.** CMake targets and `FetchContent`; how a C++ library becomes a Python extension (pybind11 and scikit-build-core); GitHub Actions; sanitizers; why fixed-point arithmetic and determinism matter in finance.

**Tasks**
1. `git init`, `.gitignore` (data, build, venvs), `.editorconfig`, LICENSE, README stub.
2. Set up WSL2 Ubuntu with the toolchain from D9. Record exact versions in the phase log.
3. Walking skeleton:
   - `engine/` static library with one function, plus one GoogleTest.
   - A pybind11 module `finforge._engine` exposing that function.
   - `uv pip install -e .` builds everything, and a pytest calls into C++.
4. CI (`.github/workflows/ci.yml`):
   - Release build and tests.
   - Debug build with ASan and UBSan, plus tests.
   - pytest.
   - `clang-format --dry-run -Werror` and `ruff check` / `ruff format --check`.
5. Write `docs/contracts.md` from §5: numeric types, time, instruments, order lifecycle, matching rules, event ordering, execution models, accounting, metrics. Each rule must be stated precisely enough to write a test against it.
6. Write the test plan: the invariants for each component (copied into `docs/contracts.md`, next to the rule each one tests).

**Exit criteria**
- [ ] On a fresh clone in WSL2, `cmake --preset dev && ctest` and `uv pip install -e . && pytest` both pass.
- [ ] CI is green on all jobs, including the sanitizer job.
- [ ] `docs/contracts.md` covers every item in §5.
- [ ] `.planning/phases/00-foundations/summary.md` is written.

---

## Phase 1 — Order Book and Matching Engine

**Objective.** A standalone, deterministic, well-tested C++20 limit order book.

**You will learn.** Price-time priority; trade-offs between data structures (tree vs hash vs array); iterator and reference invalidation; strong types; differential testing; how to benchmark without fooling yourself.

**Tasks**
1. Strong types: `Price`, `Qty`, `Money`, `OrderId`, `Timestamp`, `InstrumentId` (thin wrappers, no implicit mixing).
2. `Order`, `Trade`, the reject-reason enum, and the lifecycle transitions (§5.4).
3. `PriceLevel` (FIFO), bid side and ask side (`std::map`, best price first), and an `OrderId → locator` index.
4. Operations: `add`, `cancel`, `amend` (reduce quantity), match on add, `best_bid`, `best_ask`, `spread`, `depth(n)`, `order_status`, and trade output.
5. Order types and TIF: MARKET; LIMIT with GTC, IOC and FOK.
6. Validation and rejects, with every reason code covered by a test.
7. `ReferenceBook`: a deliberately naive, obviously correct implementation (a flat vector, scanned every time). It lives in the tests only.
8. Differential test: a seeded stream of 1M random operations runs against both books. Fills, statuses and the top of book must match after every operation.
9. Invariant checks from §5.5, asserted in debug builds and in tests.
10. Google Benchmark workloads, all with fixed seeds: add-heavy, cancel-heavy, match-heavy, mixed. Record the baseline in `docs/benchmarks.md` with the hardware and compiler.
11. `finforge-sim` CLI: reads an order script (CSV: `ts,owner,side,type,tif,price,qty` or `cancel,id`) and prints trades and the final book. This is Demo 2.

**Exit criteria**
- [ ] Unit tests cover insertion, cancellation, priority, partial and full fills, market and limit behaviour, IOC/FOK, an empty book, invalid orders and multiple price levels.
- [ ] The differential test (1M ops, 3 seeds) passes under ASan/UBSan.
- [ ] The benchmark baseline is recorded.
- [ ] The CLI demo script produces the expected output, kept as a golden file.

---

## Phase 2 — Event Engine and Exchange Simulation

**Objective.** A deterministic discrete-event engine that drives the order book. Mode B works end to end.

**You will learn.** Discrete-event simulation; `std::variant` and visitor dispatch; priority queues with stable tie-breaks; event sourcing and journals; why the standard library's random distributions aren't reproducible across platforms.

**Tasks**
1. Event types (a variant): market data (bar, quote, trade), order submit, cancel, accept, reject, fill, cancelled, timer, snapshot.
2. Simulation clock and an event queue keyed by `(ts, class, seq)` (§5.6).
3. Dispatcher plus the `ExecutionModel` interface. `BookModel` wraps the Phase 1 book.
4. Latency model (fixed ns, configurable).
5. Engine API: `run()`, `run_until(ts)`, `step()`, a progress callback every N events, and a cancel flag checked every N events.
6. A C++ `Strategy` interface for test harnesses. Python comes in Phase 5.
7. Event journal: an append-only record of every event with a stable binary encoding, plus a running SHA-256 used as the run fingerprint.
8. Synthetic order-flow generator: several agents with Poisson arrivals and random sides and prices around a mid. It uses its **own** PRNG (PCG32 or splitmix64) and its **own** uniform and exponential transforms, never the `std::` distributions.
9. Mode B multi-agent simulation from the CLI: `finforge-sim --synthetic --seed 42 --events 1000000`.

**Exit criteria**
- [ ] Same-timestamp ordering tests pass for each class combination.
- [ ] Running 1M synthetic events twice gives identical journal hashes. The same holds in Debug and Release builds.
- [ ] Book invariants hold throughout the synthetic run.
- [ ] Progress and cancel hooks are tested (a cancel stops the run within N events and leaves a consistent state).

---

## Phase 3 — Historical Data Pipeline

**Objective.** Real historical data can be ingested, validated, versioned, and replayed through the engine.

**You will learn.** Columnar storage (Arrow and Parquet, row groups); data validation; time-series pitfalls (gaps, duplicates, timezone mistakes, bar-label conventions); zero-copy numpy ↔ C++.

**Tasks**
1. Normalised Arrow schemas for **bars** (`ts_open, ts_close, instrument_id, o, h, l, c` in ticks, `volume` in lots), **quotes** (`ts, instrument_id, bid, ask, bid_qty, ask_qty`) and **trades** (`ts, instrument_id, price, qty, aggressor_side?`).
2. An instrument registry file per dataset (§5.3).
3. Ingesters: CSV and Parquet (Binance kline and bookTicker formats first, plus a generic column-mapping option). JSON only for small files.
4. Validators, which return a structured report rather than raising on the first error:
   - missing values, duplicates, non-monotonic timestamps;
   - OHLC consistency (`l ≤ o,c ≤ h`), prices ≤ 0, volume < 0, `bid ≤ ask`;
   - prices off the tick grid; gaps compared with the expected interval.
   Each dataset has a policy for each problem: reject, drop, or flag.
5. Dataset manifest: id (content hash), source, instruments, time range, row counts, validation summary, schema version.
6. `scripts/fetch_binance.py` downloads the two datasets in D5 and verifies each file against its `.CHECKSUM`. Tiny fixture datasets (a few hundred rows, including deliberately malformed ones) are committed for tests.
7. Replay: Python reads Parquet row groups and passes column arrays zero-copy to the C++ feed. The C++ side does a k-way merge across instruments (§5.6) and streams data chunk by chunk, never loading it all at once.
8. A minimal pybind11 surface: create an engine, attach feeds, run it with a no-op C++ strategy, and read event counts.

**Exit criteria**
- [ ] The sample dataset ingests, its manifest hash is stable across runs, and its validation report is clean (or flagged as expected).
- [ ] Each malformed fixture produces the expected validation findings.
- [ ] Replaying N rows across 2 instruments emits N events in the specified merge order. This is tested.
- [ ] A dataset larger than the configured chunk size replays with bounded memory (checked roughly with RSS).

---

## Phase 4 — Execution Models and Portfolio Ledger

**Objective.** Orders become fills under explicit models, and fills become exact accounting.

**You will learn.** Average-cost accounting; realised vs unrealised P&L; short-sale mechanics; transaction cost modelling; look-ahead bias; how to write golden tests for financial logic.

**Tasks**
1. `CandleModel` and `QuoteModel` exactly as specified in §5.7. The `BookModel` adapter is already done.
2. Fee models and slippage models (§5.7).
3. Pre-trade checks and reserved cash (§5.8).
4. Ledger: cash, positions, average price, realised P&L, fees, borrow cost (off by default), and the fill history.
5. Mark-to-market and equity snapshots at a configurable interval (default: every bar).
6. Reconciliation invariants (§5.8) asserted after every fill in debug builds and checked at the end of every run.
7. C++ scripted test strategies ("buy 10 at bar 3, sell 4 at bar 7, …") with **hand-computed** expected cash, positions and P&L, kept as golden tests.

**Exit criteria**
- [ ] Golden tests cover: opening a long, adding, reducing, closing, flipping long → short, opening and closing a short, partial fills, fee minimums, reserved-cash release, and rejection for insufficient cash or leverage.
- [ ] Look-ahead test: an order sent at bar *t*'s close never fills at a bar-*t* price. A strategy that "knows" the next close earns nothing from it.
- [ ] Randomised scripted runs (seeded, 1000 runs) never violate reconciliation.

---

## Phase 5 — Python Strategy SDK and End-to-End Backtest

**Objective.** A Python strategy runs on historical data through the C++ engine and produces reproducible artefacts. This completes the first full vertical slice.

**You will learn.** pybind11 trampolines, the GIL and object lifetimes across languages; designing a small, hard-to-misuse SDK; configuration validation; reproducibility manifests.

**Tasks**
1. `Strategy` base class with the hooks `on_start`, `on_bar`, `on_quote`, `on_fill`, `on_order_update` and `on_end`.
2. `Context` with: `submit_market`, `submit_limit`, `cancel`, `position(instr)`, `cash`, `equity`, `open_orders`, `history(instr, field, n)` (bounded views, §4.3), and `now`.
3. `BacktestConfig` (pydantic), with these fields:
   - dataset id, universe, start and end time;
   - initial capital, execution model, fees, slippage, latency;
   - risk limits, `allow_short`, seed;
   - strategy name and parameters.
   It loads from YAML.
4. `run(config) → RunResult`, which writes a run directory containing:
   - `manifest.json` (config, dataset hash, strategy hash, engine version and git SHA, seed, timings, journal hash, status);
   - `orders.parquet`, `fills.parquet`, `equity.parquet`.
5. CLI: `finforge backtest examples/ma_cross.yaml`.
6. Reference strategies, each with a docstring stating its logic and assumptions:
   - moving-average crossover;
   - mean reversion (z-score of price against its rolling mean);
   - time-series momentum;
   - *optional:* pairs trading (spread z-score, needs `allow_short`).
7. Failure handling: a strategy exception produces `status = failed` with the traceback in the manifest. Partial artefacts are kept and marked as partial.

**Exit criteria**
- [ ] Each of the 3 required strategies runs on the sample dataset and passes its tests (signals on crafted fixtures fire exactly where expected).
- [ ] Determinism: the same config run twice gives identical `journal_hash` and identical artefact file hashes.
- [ ] A Python-level look-ahead test passes: `history()` never returns data later than `now`.
- [ ] Reconciliation holds for every reference-strategy run.

---

## Phase 6 — Analytics and Experiment Comparison

**Objective.** Every run produces a trustworthy, structured performance and risk report, and runs can be compared.

**You will learn.** Return arithmetic and annualisation; what each risk metric assumes and when it misleads; FIFO trade reconstruction; overfitting and why out-of-sample checks matter.

**Tasks**
1. Returns from the equity curve, and every metric in §5.9.
2. Trade analytics: FIFO round trips, duration, entry and exit prices, P&L per trade, execution cost (fees plus slippage) per trade, and the holding-period distribution.
3. Exposure analytics: gross, net, concentration and capital utilisation over time, plus P&L per instrument.
4. A buy-and-hold benchmark computed for every run.
5. `report.json` written to the run directory: metrics, conventions used, and benchmark.
6. Compare N runs: a metrics table plus aligned equity and drawdown series.
7. Parameter sweep helper: a parameter grid becomes runs, executed on a multiprocessing pool (one engine per process). Each config is reported **in-sample and out-of-sample**, using a configurable holdout split by date.
8. Independent validation: hand-computed fixtures (equity series with known Sharpe, max drawdown, VaR), plus cross-checks against an independent library (for example `empyrical-reloaded`, used in tests only).

**Exit criteria**
- [ ] Every metric matches its fixture to 1e-9 (or a documented tolerance).
- [ ] Every completed run has a `report.json`.
- [ ] A 10-config sweep runs in parallel and produces a comparison table with in-sample and out-of-sample columns.

---

## Phase 7 — Backend, Persistence and Jobs

**Objective.** The engine can be used through a documented API, backtests run as background jobs, and runs are registered and retrievable.

**You will learn.** REST resource design; relational modelling and migrations; job queues built on Postgres (`SKIP LOCKED`, leases, heartbeats); SSE; failure modes and idempotency; handling input at a trust boundary.

**Tasks**
1. FastAPI app, with Postgres via `docker-compose.yml` (the dev database), SQLAlchemy 2 and Alembic migrations.
2. Schema:
   - `datasets` (manifest fields, path) and `instruments`;
   - `strategies` (the built-in registry, plus a JSON Schema for parameters);
   - `runs` (config JSONB, status, all hashes, timings, error, artefact path) and `run_metrics`;
   - `jobs` (run_id, status, attempts, `lease_until`, progress, `cancel_requested`).
3. Job queue:
   - Workers claim jobs with `FOR UPDATE SKIP LOCKED`, renew their lease while running, and execute each backtest in a child process.
   - A job whose lease has expired is marked failed, or retried if it's still within its attempt limit.
   - Cancel works by setting `cancel_requested`, which the worker passes on to the engine's cancel flag.
4. APIs:
   - **Datasets:** upload, list, get, validate.
   - **Strategies:** list, get (with the parameter schema).
   - **Runs:** submit (validated against the strategy's schema), status, cancel, results, equity, trades.
   - **Compare:** `GET /compare?runs=…`.
   - **Progress:** `GET /runs/{id}/events` (SSE).
5. Trust-boundary hardening:
   - upload size cap and type sniffing;
   - filenames sanitised and storage paths generated by the server;
   - parameters validated against the JSON Schema;
   - built-in strategies only (D8).
6. API tests against a throwaway Postgres instance, using testcontainers or the compose service.

**Exit criteria**
- [ ] A scripted HTTP workflow passes: upload → validate → submit → stream progress → fetch results and compare.
- [ ] Killing the worker during a run gets that job marked failed (or retried), and earlier runs stay unchanged.
- [ ] Cancelling a running job stops it, and the run shows `cancelled`.
- [ ] OpenAPI documentation is served at `/docs`.

---

## Phase 8 — Research Dashboard

**Objective.** The whole primary workflow (Demo 1, §13) can be done in the browser.

**You will learn.** The Next.js App Router; typed API clients generated from OpenAPI; charting time series at scale (downsampling); live progress UX with SSE.

**Tasks**
1. Next.js and TypeScript app, with a typed client generated from OpenAPI (`openapi-typescript`).
2. Pages:
   - **Overview:** recent runs, counts, datasets.
   - **Datasets:** upload, metadata, instruments, validation report.
   - **Strategies:** list, documentation, parameters.
   - **New backtest:** a form generated from the parameter JSON Schema, plus capital, costs, model and date range.
   - **Run results:** equity and benchmark, drawdown, returns histogram, trades table, price chart with trade markers, metrics and conventions, execution statistics.
   - **Compare:** metrics table, overlaid equity and drawdown, and the in-sample/out-of-sample split.
3. Live progress through SSE on the run page.
4. Server-side downsampling endpoint (LTTB or min/max per bucket) for long series.
5. Charts: Recharts for standard charts, or TradingView `lightweight-charts` for price with markers. The choice is made and logged in Phase 8.

**Exit criteria**
- [ ] Demo 1 can be done entirely in the browser.
- [ ] A one-year 1-minute equity curve renders in under 1 s (downsampled).
- [ ] A Playwright smoke test of the demo flow runs in CI.

---

## Phase 9 — Performance, Hardening and Release

**Objective.** Measured performance, a hardened system, documentation that lets anyone run it, and a v1.0 tag.

**You will learn.** Profiling (`perf`, flame graphs, heaptrack); cache-friendly data structures; object pools; measuring overhead across the language boundary; writing benchmark methodology others can reproduce.

**Tasks**
1. Profile the book and the engine on the Phase 1 and 2 workloads, and produce flame graphs.
2. Try the D11 alternatives (flat price ladder, intrusive FIFO, order pool). Keep only measured gains, and keep the differential tests green.
3. Measure the Python callback overhead (calls per second, with an empty `on_bar`) and the end-to-end backtest runtime. Document the results.
4. Profile memory on a large dataset.
5. Full `docker-compose`: db, api, worker and frontend, with one command to bring it up.
6. Docs:
   - README quickstart;
   - `docs/architecture.md` (from this plan);
   - `docs/benchmarks.md` (methodology, hardware, numbers, and how to reproduce them);
   - SDK guide with the strategy tutorial.
7. Demo dataset, scripted Demo 1 and Demo 2, and a short recorded walkthrough (optional).
8. Tag `v1.0.0`.

**Exit criteria**
- [ ] Every item in §13 is ticked.
- [ ] A fresh machine reaches the running demo by following only the README.

---

## Post-v1 Backlog (explicitly deferred)

L2/L3 historical book replay with queue-position estimation · native stop and stop-limit orders with an intra-bar policy · walk-forward optimisation · Mode B in the API and UI · equities (calendars, corporate actions, adjusted prices, survivorship) · margin calls and borrow availability · self-trade prevention · auth and multi-user · Redis/Celery or distributed sweeps · native Windows (MSVC) support.

---

# 7. Testing Strategy

| Layer | Tool | What it proves | From phase |
|---|---|---|---|
| C++ unit | GoogleTest | Each rule in `docs/contracts.md` | 1 |
| Differential | GoogleTest plus `ReferenceBook` | The optimised book behaves exactly like the naive one | 1 |
| Randomised invariants | Seeded loops in gtest | No overfill, quantity conserved, book never crossed, ledger reconciles | 1, 4 |
| Sanitizers | ASan and UBSan in CI | No memory errors or undefined behaviour | 0 |
| Golden / regression | Expected-output files | CLI output, hand-computed accounting cases | 1, 4 |
| Determinism | Journal hash comparison | Same inputs give byte-identical outputs | 2, 5 |
| Python unit and property | pytest plus hypothesis | Validators, SDK, metrics, config | 3, 5, 6 |
| Metric validation | Fixtures plus an independent library | Analytics formulas are correct | 6 |
| Integration | pytest | Dataset → replay → strategy → execution → ledger → analytics | 5 onward |
| API | pytest with a real Postgres | Endpoints, job lifecycle, failure and cancel | 7 |
| E2E | Playwright smoke test | Demo 1 in the browser | 8 |
| Benchmarks | Google Benchmark plus a timing script | Performance, with no regressions beyond the threshold | 1, 9 |

**CI runs on every push:** builds (Release and Debug with sanitizers), all unit, differential, determinism and Python tests, lint and format checks.
**Nightly or manual:** benchmarks and longer randomised runs.

---

# 8. Repository Structure

```text
finforge/
├── CMakeLists.txt              # top-level; adds engine/ and python/bindings/
├── CMakePresets.json           # dev (Debug+sanitizers), release
├── pyproject.toml              # finforge package, built via scikit-build-core
├── engine/                     # C++20 core: no Python dependency
│   ├── include/finforge/       # types.hpp, order.hpp, order_book.hpp, events.hpp,
│   │                           # engine.hpp, execution.hpp, ledger.hpp, journal.hpp, rng.hpp
│   ├── src/
│   ├── apps/finforge_sim.cpp   # Mode B CLI (Demo 2)
│   ├── tests/                  # gtest, including reference_book.hpp
│   └── bench/                  # Google Benchmark
├── python/
│   ├── bindings/               # pybind11 module finforge._engine (thin)
│   ├── finforge/
│   │   ├── data/               # schemas, ingest, validate, manifest, replay
│   │   ├── strategy/           # SDK: Strategy, Context
│   │   ├── strategies/         # reference strategies
│   │   ├── backtest/           # config, run, artefacts, CLI
│   │   └── analytics/          # metrics, trades, exposure, report, compare, sweep
│   └── tests/
├── backend/
│   ├── pyproject.toml          # depends on finforge (path dependency)
│   ├── app/{api,db,jobs,schemas}/ + main.py
│   ├── migrations/             # alembic
│   └── tests/
├── frontend/                   # Next.js + TS
├── datasets/
│   ├── fixtures/               # tiny, committed (incl. malformed)
│   └── (downloaded data)       # gitignored
├── scripts/                    # fetch_binance.py, demo scripts
├── docs/
│   ├── contracts.md            # Phase 0 spec (§5 expanded)
│   ├── architecture.md
│   └── benchmarks.md
├── .planning/                  # plan + learning journal (§12)
├── .github/workflows/ci.yml
└── docker-compose.yml
```

Phase 0 may adjust paths. Any change is logged in its `log.md`.

---

# 9. Technology Stack

| Layer | Choice | Notes |
|---|---|---|
| Core engine | C++20, STL | GCC 13+ / Clang 17+ |
| Build | CMake ≥ 3.28, Ninja, presets, FetchContent | D10 |
| C++ tests and benchmarks | GoogleTest, Google Benchmark | |
| Bindings | pybind11 + scikit-build-core | |
| Python | 3.12, `uv`, ruff, pytest, hypothesis | |
| Data | pyarrow, numpy, pandas (analysis only) | Parquet for bulk data |
| Config | pydantic v2, YAML | |
| Backend | FastAPI, SQLAlchemy 2, Alembic, psycopg 3 | |
| Database and jobs | PostgreSQL 16 (also serves as the job queue) | D7 |
| Realtime | SSE | One-way progress, so WebSockets aren't needed |
| Frontend | Next.js, TypeScript, openapi-typescript | |
| Charts | Recharts and/or lightweight-charts | Decided in Phase 8 |
| E2E | Playwright | |
| Containers | Docker, Docker Compose | Dev database in Phase 7, full stack in Phase 9 |
| CI | GitHub Actions | From Phase 0 |
| **Deferred** | Redis, Celery | Add when sweeps need multiple machines |

---

# 10. Non-Functional Requirements and Targets

**Correctness:** every invariant in §5 is enforced by tests. No order is ever overfilled. The ledger reconciles exactly. No future data reaches a strategy. Every fill can be traced to its journal event.

**Determinism and reproducibility:** identical manifests produce identical journal hashes. Every run records its config, dataset hash, strategy hash, engine version and git SHA, and seed.

**Reliability:**
- Failed runs have a clear status and error.
- Invalid data is rejected or explicitly flagged, never silently fixed.
- A worker crash never corrupts completed runs. Artefacts are written to a temp path and renamed atomically.

**Performance targets** are indicative. They are re-baselined after the first measurement in Phase 1 or 5, and documented with the hardware used.

| Workload | Target |
|---|---|
| Order book mixed operations (single thread) | ≥ 1M ops/s; p99 add < 2 µs |
| Engine, C++ no-op strategy, 1M bars | < 1 s |
| Engine, Python MA-crossover strategy, 1M bars | < 30 s |
| Ingest and validate 10M rows | < 60 s |
| Memory while replaying a dataset 10× the chunk size | Bounded, not growing with dataset size |

**Maintainability:** engine, research, backend and frontend stay separate (§4.2). Public interfaces are documented. Formatting is enforced in CI.

---

# 11. Risks and Mitigations

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Scope creep (11 modules, solo learner) | High | High | Strict phase exit criteria, the post-v1 backlog, and "each phase ships working software". |
| C++/Python build friction | Medium | High | Walking skeleton in Phase 0, WSL2 only (D9), CI from day one. |
| Silent accounting errors | Medium | High | Integer money, reconciliation invariants, hand-computed golden tests. |
| Look-ahead bias slipping in | Medium | High | Bar-close visibility rule, latency ≥ 1 ns, bounded `history()`, dedicated tests. |
| False realism (results that look tradeable but aren't) | Medium | Medium | The model name is shown on every result, assumptions are listed in the report, and in-sample vs out-of-sample results are shown. |
| Data quality or source changes | Medium | Medium | Validators, content-hash versioning, fixtures for tests, download scripts that can be re-run. |
| Premature optimisation | Medium | Medium | D11: simple first, optimise only with benchmarks (Phase 9). |
| Python callback overhead dominates runtime | Medium | Low | Measure in Phase 5 and Phase 9. Batch callbacks are a post-v1 option if needed. |
| Losing context between sessions | High | Medium | Learning journal with a "Next" pointer (§12). |

---

# 12. Learning Journal Process

The goal: at any moment you can (a) see exactly where to pick up, and (b) read a self-contained explanation of what each phase built and why.

## 12.1 Layout

```text
.planning/
├── PLAN.md                     # this document (source of truth)
├── rough-plan.md               # original draft, frozen
└── phases/
    ├── 00-foundations/
    │   ├── plan.md             # written at phase START
    │   ├── log.md              # appended DURING the phase
    │   └── summary.md          # written at phase END (marks the phase complete)
    ├── 01-order-book/
    └── …
```

**Where to resume.** The current phase is the lowest-numbered folder that has no `summary.md`. Resume from the **Next** line of the last entry in its `log.md`. No separate state file is needed.

## 12.2 `plan.md` (phase start)

```markdown
# Phase NN — <Name>
## Goal            (copied from PLAN.md, refined)
## Learning goals  (concepts to understand by the end)
## Tasks           (- [ ] checklist; tick as done)
## Exit criteria   (copied from PLAN.md)
## Open questions / risks
```

## 12.3 `log.md` (append-only during the phase)

One entry per work session, newest at the bottom:

```markdown
## YYYY-MM-DD — <short title>
**Done:** what changed (files/modules), tests added
**Decisions:** <decision> — because <reason> (alternatives considered)
**Problems:** what broke, root cause, fix
**Learned:** concepts/insights worth remembering
**Next:** the exact next step (first thing to do when resuming)
```

## 12.4 `summary.md` (phase end)

Write it for your future self as a teaching document:

```markdown
# Phase NN — <Name>: Summary
## What was built           (components, public interfaces, where they live)
## How it works             (walkthrough of the main flow, with a diagram if useful)
## Key decisions            (decision · rationale · alternatives · consequences)
## Concepts explained       (the theory behind it: e.g. price-time priority, avg-cost accounting)
## Testing                  (what is tested, how, and what the invariants guarantee)
## Results                  (benchmarks, demo output, exit-criteria evidence)
## Gotchas & lessons        (mistakes made and how to avoid them)
## Deviations from PLAN.md  (and whether PLAN.md was updated)
## Open items carried forward
```

## 12.5 Rules

- Decisions that change this plan (contracts, architecture, scope) are also recorded in §15 here.
- Product documentation (`docs/`) describes how the system **is**. The journal (`.planning/phases/`) describes how it **got that way** and what was learned along the way.

---

# 13. Definition of Done (v1.0)

**Engine**
- [ ] C++20 order book with price-time priority; MARKET, LIMIT, GTC/IOC/FOK, cancel, amend (reduce), partial fills.
- [ ] Passes differential and invariant tests against `ReferenceBook` under sanitizers.
- [ ] Deterministic event engine: identical journal hashes on repeated runs.
- [ ] Mode B CLI (Demo 2) with scripted and seeded synthetic order flow.

**Data and backtesting**
- [ ] Historical data is ingested, validated, versioned by hash, and replayed across multiple instruments in chunks.
- [ ] Candle and Quote execution models implemented exactly as `docs/contracts.md` specifies.
- [ ] Fees, slippage, pre-trade checks and shorts are modelled. The ledger reconciles exactly on every run.
- [ ] Python SDK, plus at least 3 tested reference strategies.
- [ ] Every run produces a manifest and artefacts, and is reproducible from its manifest alone.

**Analytics**
- [ ] All metrics in §5.9 are validated against fixtures, with a buy-and-hold benchmark and trade and exposure analytics.
- [ ] Run comparison and parameter sweeps, with in-sample and out-of-sample reporting.

**Platform**
- [ ] FastAPI service with dataset, strategy, run and compare APIs, plus a Postgres job queue with cancel, progress and crash recovery.
- [ ] Dashboard supports the full Demo 1 workflow.
- [ ] One-command Docker Compose stack.

**Quality**
- [ ] CI is green: builds, sanitizers, unit, integration, determinism, E2E smoke test and lint.
- [ ] Benchmarks documented with methodology and hardware.
- [ ] README quickstart, architecture, contracts and SDK docs are complete.
- [ ] Every phase has a `summary.md`.

**Demo 1:** pick a dataset → MA crossover → set parameters, capital, fees, slippage and model → start → watch progress → inspect equity against the benchmark, trades, drawdown and risk → compare with a second parameter set.
**Demo 2:** `finforge-sim` matches scripted and synthetic orders using price-time priority, with a deterministic hash.

---

# 14. Open Questions (defaults chosen, so work can proceed)

| # | Question | Default used in this plan |
|---|---|---|
| Q1 | Develop in WSL2 or in native Windows (MSVC)? | WSL2 (D9) |
| Q2 | Is the primary market crypto (Binance) or equities? | Crypto first (D5); equities post-v1 |
| Q3 | Solo project or a team? (The rough plan mentions "collaboration".) | Solo; PR-based workflow still used for history |
| Q4 | Will the repository be public? (This affects the license and data redistribution.) | Public, MIT license, no raw data committed |

Changing a default means a §15 entry and updates to the affected sections.

---

# 15. Change Log

| Date | Version | Change |
|---|---|---|
| 2026-11-11 | 1.0 | Formal plan created from `rough-plan.md`. Fixed gaps G1–G27 (§1). Restructured the roadmap (portfolio and execution models moved before the Python SDK; experiment management placed in Phases 5–7; CI moved to Phase 0). Pinned the domain contracts. Removed Redis and Celery from v1. Added the learning journal process. |
| 2026-11-11 | 1.1 | Q1–Q4 defaults confirmed by the user. D5 corrected: spot has no bookTicker, so quotes come from USD-M futures bookTicker (available 2023-05-16 → 2024-03-30). |
| 2026-11-13 | 1.2 | Phase 0: `docs/contracts.md` written. Gaps in §5 filled as refinements (contracts.md §12): `Rate` type at 1e-6, `money_per_tick_lot` must be an integer, overflow is a hard failure, cost-basis ledger, total-order event key, Quote fills all taker, extra cancel reasons. §5 decisions unchanged. |
