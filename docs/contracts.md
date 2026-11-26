# FinForge Contracts

This is the specification that the engine and the research layer are tested against. It expands
PLAN.md §5. If code and this document disagree, the code is wrong or this document is changed
on purpose (with a PLAN.md §15 entry if it changes a §5 default).

**How to read it.**
- Every rule has an ID (`NUM-3`, `ORD-7`, …). Tests reference rules by ID in their name or a comment,
  so `grep -r ORD-7` finds the tests for a rule.
- **MUST** means the behaviour is required and tested. Each rule ends with **Test:** saying how it is
  verified and from which phase.
- Rules marked **(refinement)** pin down a detail that PLAN.md §5 left open. They are listed in §12.
- Points that are deliberately not decided yet are in §13 with the phase that must decide them.

---

## 1. Numeric types

| Type | Representation | Meaning |
|---|---|---|
| `Price` | `int64` ticks | `real = ticks × tick_size` |
| `Qty` | `int64` lots | `real = lots × lot_size` |
| `Money` | `int64`, scale 10⁻⁸ of the quote currency | `real = units × 1e-8` |
| `Rate` | `int64`, scale 10⁻⁶ (one unit = 0.01 bp) | `real = units × 1e-6`; 10 bps = `1000` |
| `OrderId`, `TradeId`, `Seq` | `uint64` | |

- **NUM-1** The engine MUST NOT use floating point for prices, quantities, money, rates, or anything
  that feeds a fill, a ledger entry or a journal event. Doubles exist only at the edges (ingestion
  converts in, analytics converts out).
  **Test:** code review + a CI grep that forbids `float`/`double` in `engine/src` outside an allow-list (Phase 1).
- **NUM-2** Every instrument defines the integer `money_per_tick_lot = tick_size × lot_size / 1e-8`.
  It MUST be an exact integer ≥ 1; otherwise the instrument is rejected when the registry loads.
  **(refinement)** Examples: BTCUSDT spot (tick 0.01, lot 0.00001) → 10; ETHUSDT spot (0.01, 0.0001) → 100;
  BTCUSDT USD-M futures (0.1, 0.001) → 10 000. The Phase 3 dataset registers BTCUSDT spot with lot
  0.000001 (→ 1), because some 2024 kline volumes have six decimals.
  **Test:** unit tests for accept/reject of tick/lot pairs (Phase 1).
- **NUM-3** Notional MUST be computed as `price_ticks × qty_lots × money_per_tick_lot × contract_multiplier`
  in 128-bit arithmetic, then range-checked into `int64` Money.
  **Test:** unit tests at the boundaries, including values that overflow 64 bits mid-computation (Phase 1).
- **NUM-4** Overflow MUST never wrap. A result that does not fit `int64` is a hard error: the run
  stops with status `FAILED` and reason `NUMERIC_OVERFLOW`. **(refinement)**
  **Test:** unit test that forces overflow; UBSan in CI catches signed-overflow UB elsewhere (Phase 1).
- **NUM-5** Rounding: fees round **up** to the next Money unit (conservative). Slippage-adjusted
  prices round to the tick grid **against** the order (buy up, sell down). Cost-basis release
  rounds toward zero (ACC-3). No other rounding exists; everything else is exact.
  **Test:** hand-computed golden cases for each rounding site (Phase 4).
- **NUM-6** `Qty` on an order MUST be > 0. Limit `Price` MUST be > 0.
  **Test:** reject tests (`INVALID_QTY`, `INVALID_PRICE`) (Phase 1).
- **NUM-7** `OrderId`, `TradeId` and `Seq` are assigned from per-run counters that start at 1 and
  increase by 1. They MUST never be random or derived from wall-clock time.
  **Test:** IDs are dense and increasing in every randomised run (Phase 1).

## 2. Time

- **TIME-1** `Timestamp` is `int64` nanoseconds since the Unix epoch, UTC. No local time exists anywhere
  in the engine.
  **Test:** type-level; ingestion tests convert known source timestamps exactly (Phase 3).
- **TIME-2** Bars are labelled by open time. `close_time = open_time + interval`. A bar's event is
  emitted at `close_time`.
  **Test:** replay test checks event timestamps for a fixture (Phase 3).
- **TIME-3** A strategy MUST NOT observe bar *t* (any field) before the simulation clock reaches
  `close_time(t)`. `history()` returns only bars with `close_time ≤ now`.
  **Test:** look-ahead test with a strategy that asserts on every call (Phase 5).
- **TIME-4** Quotes and trades are emitted at their exchange timestamp.
  **Test:** replay fixture (Phase 3).
- **TIME-5** The simulation clock is monotonic non-decreasing and never reads wall-clock time.
  **Test:** assertion in the event loop (debug builds) + randomised runs (Phase 2).
- **TIME-6** Source timestamps are converted at ingestion only. Binance klines/bookTicker for 2023–2024
  are in milliseconds; Binance switched spot files to microseconds from 2025-01-01. The ingester MUST
  detect the unit per file and reject ambiguous files rather than guess. **(refinement)**
  Detection: a value's unit is the one whose plausible range (2000-01-01 to 2100-01-01) contains it;
  the s / ms / µs / ns ranges do not overlap. The first usable value fixes the file's unit; any value
  outside that unit's range is `TS_UNIT_AMBIGUOUS` and the dataset is rejected. Binance formats allow
  only ms and µs (docs/data.md).
  **Test:** fixtures in both units plus one ambiguous file (Phase 3).

## 3. Instruments

- **INS-1** An instrument has `instrument_id` (dense `uint32` from 0), `symbol`, `asset_class`,
  `quote_currency`, `tick_size`, `lot_size`, `min_qty` (lots), `contract_multiplier` (1 for spot).
  `tick_size` and `lot_size` are stored as decimal strings, never as doubles. **(refinement)**
- **INS-2** The registry is stored in the dataset manifest. A run uses the registry of its dataset.
- **INS-3** An order for an unknown instrument is rejected `UNKNOWN_INSTRUMENT`. A price that is not a
  whole number of ticks is rejected `OFF_TICK`; a quantity that is not a whole number of lots is
  rejected `OFF_LOT`; a quantity below `min_qty` is rejected `INVALID_QTY`. (`OFF_TICK`/`OFF_LOT` only
  arise where real-valued inputs are converted, i.e. in the Python SDK; inside the engine prices and
  quantities are already integers.)
  **Test:** reject tests (Phases 1, 5).
- **INS-4** The USD-M BTCUSDT perpetual used for quotes is treated as spot-like: multiplier 1, funding
  ignored (PLAN.md D5). This is shown in every report that uses it.

## 4. Orders and lifecycle

Fields: `id`, `owner_id`, `instrument_id`, `side` {BUY, SELL}, `type` {MARKET, LIMIT},
`tif` {GTC, IOC, FOK}, `price` (LIMIT only), `qty`, `remaining`, `submit_ts`, `seq`, `status`.

```text
            ┌──────────► REJECTED (terminal; reason code)
 NEW ───────┤
            └──► ACCEPTED ──► PARTIALLY_FILLED ──► FILLED (terminal)
                    │               │
                    └───────────────┴──► CANCELLED (terminal; reason code)
```

- **ORD-1** The only allowed transitions are those in the diagram (plus PARTIALLY_FILLED →
  PARTIALLY_FILLED on further partial fills, and ACCEPTED → FILLED when a single fill completes
  the order **(refinement)**). Any other transition is a bug and MUST trip an assertion.
  **Test:** state-machine unit tests; assertion active in all test builds (Phase 1).
- **ORD-2** Every transition emits exactly one event.
  **Test:** event count equals transition count in randomised runs (Phase 1–2).
- **ORD-3** Terminal states (REJECTED, FILLED, CANCELLED) never change. An action on a terminal order is
  answered with `ALREADY_TERMINAL`; on an unknown id with `UNKNOWN_ORDER`.
  **Test:** unit tests (Phase 1).
- **ORD-4** Reject reasons are exactly: `INVALID_QTY`, `INVALID_PRICE`, `OFF_TICK`, `OFF_LOT`,
  `UNKNOWN_INSTRUMENT`, `INSUFFICIENT_CASH`, `POSITION_LIMIT`, `LEVERAGE_LIMIT`, `SHORT_NOT_ALLOWED`,
  `UNKNOWN_ORDER`, `ALREADY_TERMINAL`.
- **ORD-5** Cancel reasons are exactly: `USER`, `IOC_REMAINDER`, `FOK_UNFILLABLE`, `MARKET_REMAINDER`,
  `END_OF_RUN`, `INSUFFICIENT_CASH_AT_FILL`. **(refinement)** — §5.4 named the first three; the others
  make every cancellation traceable (`INSUFFICIENT_CASH_AT_FILL`: OP-1, Phase 4).
  **Test:** each reason produced by a unit test (Phases 1, 4).
- **ORD-6** MARKET orders have no price; a MARKET order with a price is rejected `INVALID_PRICE`.
  MARKET orders never rest: in every model their unfilled remainder is cancelled `MARKET_REMAINDER`.
  `tif` is ignored for MARKET. **(refinement)**
- **ORD-7** Amend: reducing quantity (new qty > filled qty, < current qty) keeps queue priority.
  Changing the price or increasing quantity is cancel-and-replace: the old order is CANCELLED (`USER`),
  a new order with a new `id` and `seq` is submitted, and it loses priority. "new qty" is the new
  **total** quantity, so the replacement is for `new qty − filled` **(refinement)**. An amend to the
  same price and quantity is a no-op; `new qty ≤ filled` is refused `INVALID_QTY` and a price ≤ 0
  `INVALID_PRICE`, without changing the order. A MARKET order still waiting for its bar (Candle)
  keeps price 0: any other price is refused `INVALID_PRICE` (ORD-6). Account checks (ACC-9) on a
  replacement no longer count the order it replaces.
  **Test:** priority tests in the book (Phase 1).
- **ORD-8** Quantity conservation: at all times `filled + remaining + cancelled_qty = qty`.
  **Test:** invariant checked after every operation in randomised runs (Phase 1).

## 5. Matching rules (Mode B, `BookModel`)

- **MAT-1** Price-time priority: better price first; at equal price, lower `seq` first.
- **MAT-2** The trade price is the **resting** order's price.
- **MAT-3** A MARKET order matches available liquidity; its remainder is cancelled (ORD-6).
- **MAT-4** A marketable LIMIT order matches first. Its remainder rests (GTC), is cancelled (IOC,
  `IOC_REMAINDER`), or — for FOK — the whole order is cancelled `FOK_UNFILLABLE` **before any fill** if
  it cannot fill completely at prices within its limit.
- **MAT-5** Fully filled resting orders are removed. After any operation completes, the book is not
  crossed: `best_bid < best_ask` whenever both sides exist.
- **MAT-6** No self-trade prevention in v1; `owner_id` is recorded on every fill.
- **MAT-7** Every fill references one aggressor and one resting order, both existing, on opposite
  sides, same instrument. Fill qty > 0. No order is ever overfilled.
- **MAT-8** Price levels with zero resting quantity do not exist in the book.

**Tests (Phase 1):** a unit test per rule; differential testing against a naive `ReferenceBook`
(same operation stream ⇒ identical fills, events and book state); seeded randomised streams checking
ORD-8 and MAT-5/7/8 after every operation; all under ASan/UBSan.

## 6. Event ordering and latency

- **EVT-1** The event queue is ordered by the key `(ts, class, instrument_id, seq)` where class is
  `0` exchange actions due (order arrivals, cancels, amends), `1` market data, `2` timers/snapshots,
  and `seq` is a global per-run insertion counter. **(refinement:** `instrument_id` and `seq` make the
  key a total order.)
- **EVT-2** Multi-instrument data with equal timestamps is merged by `instrument_id`, then source row
  order (deterministic k-way merge). **(refinement:** the merge key is `(emit time, instrument_id,
  feed, row)`, where a feed is one (kind, instrument) file and feeds of one instrument are ordered
  bars < quotes < trades. Replay streams the merged data into the queue one event at a time, so at
  equal `(ts, class, instrument_id)` the merge order is the processing order.)
- **EVT-3** An order submitted while handling an event at time `t` arrives at the execution model at
  `t + latency`. Default `latency = 1 ns`, configurable, MUST be ≥ 1 ns. Therefore an order can never
  trade against the event that triggered it.
- **EVT-4** Events are processed strictly in key order; an event may only schedule events with a key
  greater than its own.

**Tests (Phase 2):** ordering unit tests for every tie-break; a latency test where a strategy reacts to
a quote and must not fill against that same quote; property test that the processed key sequence is
strictly increasing.

## 7. Execution models (Mode A)

All models implement one `ExecutionModel` interface (PLAN D4). Every result records the model name and
its parameters.

### 7.1 CandleModel (bars)

- **CND-1** An order is eligible on bar *b* iff `open_time(b) ≥ sent_ts`, where `sent_ts` is the
  strategy's clock when it sent the order (arrival is `sent_ts + latency`). An order submitted on bar
  *t*'s close event is first eligible on bar *t+1*; one sent mid-bar (from a timer) on the bar after
  next. **(refinement, Phase 4:** the earlier wording keyed eligibility to the *arrival* time, which with
  latency ≥ 1 ns lands just after `close_time(t)` and contradicted its own example. Keying it to the send
  time keeps the intended "decide at t's close, fill at t+1's open" and never fills an order at a price
  from before the strategy decided.)
- **CND-2** MARKET: fills at `open[t+1]` adjusted by slippage (§8), for the full quantity unless the
  volume cap (CND-5) applies.
- **CND-3** LIMIT BUY fills if `low[t+1] < limit` (strict trade-through). Fill price
  `min(open[t+1], limit)`. LIMIT SELL fills if `high[t+1] > limit`, at `max(open[t+1], limit)`.
  No slippage on limit fills. **(refinement)**
- **CND-4** TIF: GTC orders stay working and are evaluated on every later bar; IOC orders are evaluated
  on their first eligible bar only, then any remainder is cancelled `IOC_REMAINDER`; FOK fills completely
  on its first eligible bar or is cancelled `FOK_UNFILLABLE` with no fill.
- **CND-5** Optional volume cap, off by default: `fill_qty ≤ floor(participation × volume[t+1])` lots,
  `participation` a `Rate`. The cap is shared by all of the strategy's orders on that instrument and bar,
  allocated in order `seq`. **(refinement)**
- **CND-6** No native stops in v1.
- **CND-7** All Candle fills are taker fills.

### 7.2 QuoteModel (L1 bid/ask)

- **QTE-1** A MARKET BUY fills at the first ask with timestamp ≥ arrival, quantity capped at that
  quote's ask size; the remainder is cancelled (ORD-6). SELL mirrors against the bid.
- **QTE-2** A LIMIT BUY fills when a quote has `ask ≤ limit`, at the ask, capped at the ask size.
  GTC remainders keep working on later quotes; IOC is evaluated on the first quote at or after arrival
  only; FOK fills completely on that quote or is cancelled. SELL mirrors with `bid ≥ limit`.
- **QTE-3** The size of one quote is consumed across all of the strategy's orders on that instrument
  in `seq` order; the same displayed size is never filled twice. **(refinement)**
- **QTE-4** All Quote fills are taker fills in v1, because there is no queue model. **(refinement —
  PLAN §5.7 mentioned maker/taker for QuoteModel; without queue position, a maker fill can't be justified.)**

### 7.3 BookModel

- **BKM-1** Delegates to the order book (§5). Aggressor fills pay taker fees, resting fills pay maker fees.
  A LIMIT buy is checked at its limit, which is a true worst case because book fills happen at resting
  prices no worse than the limit. A MARKET buy is checked at the mark (the book mid). There is no
  fill-time cap (OP-1) in Mode B.

**Tests (Phase 4):** a hand-computed golden case per rule, including the boundary cases
`low == limit` (no fill) and `ask == limit` (fill).

## 8. Fees and slippage

- **FEE-1** Fee per fill = `max(ceil(notional × rate / 1e6), min_fee)` in Money, where `rate` is the
  maker or taker `Rate` and `min_fee` defaults to 0. A fill with notional > 0 and rate > 0 always pays
  ≥ 1 Money unit.
- **SLP-1** Slippage models: `none`; `fixed_bps` (price × (1 ± bps)); `half_spread` (Candle only,
  price × (1 ± spread_bps / 2)). Slippage is always adverse. The adjusted price is rounded to the tick
  grid against the order (NUM-5).
- **SLP-2** Slippage applies only to MARKET fills in the Candle model. Quote and Book fills are at quoted
  or resting prices. **(refinement)**

**Tests (Phase 4):** golden cases for each model, including rounding at the tick boundary.

## 9. Portfolio accounting and pre-trade checks

Per instrument the ledger holds signed `position` (lots) and `cost_basis` (Money, signed with the
position). Account-level: `cash`, `reserved_cash`, `realised_pnl`, `total_fees`, `borrow_costs`.
**(refinement:** the average price is `cost_basis / position`, derived for display; storing the basis
instead of an average price keeps everything exact.)

- **ACC-1** Fill that increases |position|: `cost_basis += signed notional`; `cash -= signed notional`.
- **ACC-2** Fees are debited from cash at the fill: `cash -= fee`; `total_fees += fee`.
- **ACC-3** Fill that reduces |position| by `q` lots out of `|p|`: released basis
  `b = trunc(cost_basis × q / |p|)` (toward zero); `realised_pnl += signed proceeds − b`;
  `cost_basis -= b`. When the position reaches 0, the whole remaining basis is released, so rounding
  never leaves residue.
- **ACC-4** A fill that crosses zero (flip) is processed as a close of the full position followed by an
  open of the remainder at the same price, within one fill event.
- **ACC-5** Short-sale proceeds are credited to cash.
- **ACC-6** Mark price: last close (Candle); `floor((bid + ask) / 2)` ticks (Quote, Book). **(refinement)**
  `market_value = position × mark × money_per_tick_lot × multiplier`;
  `unrealised_pnl = Σ (market_value − cost_basis)`; `equity = cash + Σ market_value`.
- **ACC-7** Reserved cash: an open LIMIT BUY reserves `limit × remaining` notional plus the estimated
  taker fee on it; the reservation is released on fill (pro rata) or cancel.
- **ACC-8** Shorts are allowed only when `allow_short = true`. Borrow cost (default 0) accrues at each
  UTC midnight: `ceil(|short market_value| × borrow_rate / 1e6 / 365)`, `borrow_rate` an annual `Rate`.
  No margin calls in v1.
- **ACC-9** Pre-trade checks run in this order, the first failure is the reject reason:
  instrument → tick/lot → qty > 0 → cash for buys (`cash − reserved_cash ≥` order notional + fee at
  the reference price) → position limit → `allow_short` → gross leverage after the order
  `Σ|market_value| / equity ≤ max_leverage` (default 1.0). For MARKET orders the reference price is
  the current mark adjusted by the configured slippage. **(refinement, Phase 4:** position limit,
  short and leverage checks use the *projected* position: position + the remaining quantity of the
  account's open orders + this order; leverage divides by equity minus the estimated fee. A MARKET
  order with no mark yet is checked at price 0 and capped at fill time (OP-1). The checks apply to
  amend replacements too. Owners without an account are not checked.)
- **ACC-10 (reconciliation)** After every fill (debug builds) and at the end of every run:
  `equity == initial_cash + realised_pnl + unrealised_pnl − total_fees − borrow_costs`, exactly.
- **ACC-11 (replay)** Replaying the fill ledger from scratch reproduces final cash, positions and cost
  bases exactly.

**Tests (Phase 4):** hand-computed golden cases (open, add, partial close, full close, flip, short,
fees, borrow accrual); ACC-10/11 checked in seeded randomised runs; one reject test per pre-trade check
and one test for their order.

## 10. Determinism and versioning

- **DET-1** Every run writes an event journal. Its SHA-256 is the run fingerprint. Same inputs
  (dataset hash, strategy hash, config, seed, engine version) MUST give an identical fingerprint.
  **Test:** run twice, compare hashes — in CI (Phases 2, 5).
- **DET-2** Randomness comes only from the engine's own seeded PRNG (splitmix64/PCG) with its own
  distribution transforms; `std::*_distribution` is not used (PLAN G22).
  **Test:** fixed seed ⇒ fixed known output sequence, checked against constants (Phase 2).
- **DET-3** Iteration order never depends on pointer values or `unordered_*` iteration order when it
  can affect output. **Test:** determinism test under ASan (different addresses) (Phase 2).
- **VER-1** Dataset version = SHA-256 of the normalised content **(refinement:** of the logical
  column values, schema version and registry, not of the Parquet bytes, which change with the
  pyarrow version; docs/data.md §5). Strategy version = SHA-256 of
  the strategy source plus name and parameters. Engine version = `<semver>+<git sha>.<build type>`
  (`finforge.engine_version()`, Phase 0). All three go in every run manifest.

### 10.1 Journal format (OP-2, decided in Phase 2)

The journal is what DET-1 hashes, so its bytes are part of the contract.

```text
header : "FFJ1" (4 bytes) | u16 format_version (= 1) | u16 reserved (= 0)
record : u8 type | u8 reserved (= 0) | u16 payload_length | payload
key    : i64 ts | u8 class | u32 instrument_id | u64 seq          (EVT-1 key)
```

- Every integer is **little-endian, fixed width**, no padding, no floating point. Enums are `u8`
  with the numeric values of the engine enums.
- **Input records** (each processed queue event, in processing order), payload starts with `key`:
  `1 Submit` (owner u32, client_ref u64, instrument u32, side u8, type u8, tif u8, price i64,
  qty i64) · `2 Cancel` (owner u32, order u64) · `3 Amend` (owner u32, order u64, qty i64,
  price i64) · `4 Bar` (instrument u32, open_time i64, close_time i64, open i64, high i64, low i64,
  close i64, volume i64) · `5 Quote` (instrument u32, ts i64, bid i64, bid_qty i64, ask i64,
  ask_qty i64) · `6 TradePrint` (instrument u32, ts i64, price i64, qty i64, aggressor_side u8:
  0 buy, 1 sell, 2 unknown — added in Phase 3; no earlier journal contains a 2, so the format
  version is unchanged) · `7 Timer` (owner u32, tag u64) ·
  `8 Snapshot` (per book: instrument u32, has_bid u8, bid i64, bid_qty i64, has_ask u8, ask i64,
  ask_qty i64).
- **Output records** (what the execution model emitted, right after the input that caused them):
  `16 OrderUpdate` (order u64, status u8, filled i64, remaining i64, reject_reason u8,
  cancel_reason u8) · `17 OrderAmended` (order u64, qty i64, remaining i64) · `18 Trade` (trade_id
  u64, instrument u32, price i64, qty i64, aggressor u64, resting u64, aggressor_owner u32,
  resting_owner u32, aggressor_side u8, ts i64) · `19 ActionRefused` (key, owner u32, order u64,
  reason u8).
- The engine version is **not** in the journal (it differs between Debug and Release builds by
  design); it goes in the run manifest (VER-1).
- Any change to this layout bumps `format_version` and changes every fingerprint.
- **Test:** golden bytes for a record; the hash equals SHA-256 of the sink bytes; a 1M-event
  synthetic run has a pinned fingerprint in every CI build (Phase 2).

## 11. Metrics conventions

Computed in Python from the engine's exported arrays (PLAN D1).

- **MET-1** *Fill* = one execution. *Trade* = one FIFO match between an opening lot and closing
  quantity of the same instrument; its P&L is net of the fees of both fills, pro rata by quantity.
- **MET-2** Returns: simple returns of equity sampled at each UTC day's close (the last equity value at
  or before 24:00:00 UTC). The first return is against `initial_cash`. Days with no events carry the
  previous equity forward.
- **MET-3** `N = periods_per_year`, default 365 (252 for equities). Risk-free: configurable annual rate,
  converted per period as `(1 + rf)^(1/N) − 1`, default 0.
- **MET-4** Sharpe = `mean(excess) / std(excess, ddof=1) × √N`. Undefined (reported as null with a
  flag) if std = 0 or fewer than 2 periods.
- **MET-5** Sortino = `mean(excess) / downside_dev × √N`, `downside_dev = √(mean(min(r, 0)²))` over all
  periods. Infinite (flagged) if downside_dev = 0.
- **MET-6** Volatility = `std(r, ddof=1) × √N`.
- **MET-7** Max drawdown = largest `(peak − trough) / peak` of the equity curve, reported as a positive
  fraction, with its duration (peak to recovery, or to the end if not recovered).
- **MET-8** VaR / CVaR: historical, 1-day, 95%, reported as positive loss fractions.
- **MET-9** Profit factor = gross profit / gross loss over trades; infinite (flagged) if no losses.
  Win rate = winning trades / closed trades, net of fees.
- **MET-10** Benchmark = equal-weight buy-and-hold of the same universe, same capital, fees and
  execution model.

**Tests (Phase 6):** every metric against hand-computed fixtures and an independent library; edge
cases (flat equity, no trades, all wins). Done: `python/tests/test_metrics.py` (1e-9, incl.
empyrical-reloaded), `test_analytics.py`; definitions in [analytics.md](analytics.md).

---

## 12. Refinements to PLAN.md §5

Details chosen while writing this document. None changes a §5 decision; they fill gaps. Recorded in
PLAN.md §15 v1.2.

| ID | Refinement |
|---|---|
| NUM-2, INS-1 | `money_per_tick_lot` must be an exact integer; tick/lot stored as decimal strings |
| NUM-4 | Overflow is a hard run failure, never wraps |
| `Rate` type | Fee, slippage, borrow and participation rates are `int64` at 1e-6 |
| NUM-5, SLP-1 | Slippage-adjusted prices round to the tick grid against the order |
| TIME-6 | Timestamp unit detected per file at ingestion (Binance ms → µs change in 2025) |
| ORD-5 | Cancel reasons extended with `MARKET_REMAINDER`, `END_OF_RUN` |
| ORD-6 | MARKET orders never rest in any model; `tif` ignored |
| EVT-1 | Queue key extended to `(ts, class, instrument_id, seq)` — a total order |
| CND-3, SLP-2 | No slippage on limit fills; slippage only on Candle MARKET fills |
| CND-5, QTE-3 | Volume cap / quote size shared across a strategy's orders in `seq` order |
| QTE-4 | Quote fills are all taker in v1 |
| §9, ACC-3 | Ledger stores cost basis (Money), not average price; release truncates, zero position releases all |
| ORD-1 | ACCEPTED → FILLED is legal (one fill completes the order) — Phase 1 |
| ORD-7 | Amend qty is the new total; replacement is for `new qty − filled`; same price+qty is a no-op — Phase 1 |
| ACC-6 | Quote/Book mid mark is `floor((bid+ask)/2)` |
| TIME-6 | Unit detected from non-overlapping plausible ranges; first usable value fixes the file's unit — Phase 3 |
| EVT-2 | Merge key `(emit time, instrument_id, feed, row)`; feeds of one instrument: bars < quotes < trades — Phase 3 |
| VER-1 | Dataset id hashes the logical content, not Parquet bytes — Phase 3 |
| CND-1 | Eligibility keyed to the send time: `open_time(b) ≥ sent_ts` — Phase 4 |
| ORD-5 | New cancel reason `INSUFFICIENT_CASH_AT_FILL` (OP-1) — Phase 4 |
| ACC-9 | Projected position (incl. open orders) for limit/short/leverage; amend replacements checked — Phase 4 |
| MET-2/7/8 | Midnight points close the ended day; drawdown on the full curve with initial capital first, plus longest time under water; VaR quantile = linear interpolation — Phase 6 |
| MET-1 | Execution cost per trade = pro-rata fees + adverse difference to a reference price (Candle: bar open, Quote: mid) — Phase 6 |

## 13. Open points (decide by the phase shown)

| # | Question | Proposed default | Decide by |
|---|---|---|---|
| OP-1 | A MARKET buy passes the pre-trade cash check at the reference price, but fills at a worse price than cash allows. | **Decided (Phase 4):** in the Candle and Quote models, any buy is cut to the whole lots that `cash − other reservations` pays for, fee included; the rest is cancelled `INSUFFICIENT_CASH_AT_FILL` (a FOK order is cancelled whole). | Phase 4 |
| OP-2 | Journal byte format (field order, endianness) for DET-1. | **Decided (Phase 2):** little-endian fixed-width records with a versioned header — §10.1. | Phase 2 |
| OP-3 | Orders still open at the end of a run. | **Decided (Phase 4):** `Simulator::finish()` cancels them with `END_OF_RUN`, then runs the final reconciliation. | Phase 4 |

## 14. Test plan by component

| Component | Invariants / properties | Rules | Test kind | Phase |
|---|---|---|---|---|
| Numeric core | No wrap; exact notional; rounding sites | NUM-1…7 | Unit, boundary | 1 |
| Order state machine | Legal transitions only; one event each; terminal is final; qty conserved | ORD-1…8 | Unit + randomised | 1 |
| Order book | Price-time priority; resting price; never crossed; no empty levels; no overfill; matches `ReferenceBook` | MAT-1…8 | Unit, differential, randomised, sanitizers | 1 |
| Event engine | Total order; monotonic clock; latency ≥ 1 ns; no same-event fills | EVT-1…4, TIME-5 | Unit + property | 2 |
| Determinism | Same inputs ⇒ same journal hash; PRNG fixed sequence | DET-1…3 | Determinism test in CI | 2, 5 |
| Data pipeline | Exact timestamp and decimal conversion; bars emitted at close; malformed data reported per fixture; merge order; bounded-memory replay | TIME-1,2,4,6, INS-1,2, EVT-2, VER-1 | pytest + fixtures, gtest | 3 |
| Execution models | Fill rules per model; boundary cases; TIF; caps; look-ahead | CND-*, QTE-*, BKM-1, FEE-1, SLP-*, OP-1 | Golden cases, look-ahead tests | 4 |
| Ledger | Reconciliation exact; replay reproduces state; pre-trade check order | ACC-1…11, OP-3 | Golden, scripted end-to-end, 1000 seeded runs | 4 |
| Strategy SDK | No look-ahead (`history()` ≤ now); off-tick/off-lot rejected at conversion; same config ⇒ same journal and artefacts; reference strategies reconcile | TIME-3, INS-3, DET-1, ACC-10/11 | pytest (crafted fixtures, real sample locally) | 5 |
| Analytics | Every metric equals fixture and independent library | MET-1…10 | pytest + fixtures | 6 |
| Versioning | Manifest contains all three versions | VER-1 | pytest | 0 (engine version), 5 |
