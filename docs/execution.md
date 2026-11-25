# Execution models and accounting

How orders become fills and how fills become exact accounting (PLAN.md Phase 4). The rules
themselves are in [contracts.md](contracts.md): §7 (models), §8 (fees and slippage), §9 (ledger).
This page explains how the pieces fit together and how to use them from C++. The Python SDK arrives
in Phase 5.

## 1. Components

```text
Strategy ──Context::submit──► Simulator ──(arrival = sent + latency)──► ExecutionModel
                                  │                                        │  BookModel   (Mode B)
                                  │ advance(now)                           │  CandleModel (bars)
                                  │ after_market_data                      │  QuoteModel  (L1)
                                  ▼                                        │
                               Accounts ◄──────── AccountHooks ────────────┘
                               one Ledger per owner: pre_trade · affordable · on_fill · on_order_update
```

| Piece | Header | Role |
|---|---|---|
| `ExecutionModel` | `execution_model.hpp` | Order entry, `on_market_data`, events, `mark()` (ACC-6), `cancel_all` |
| `BookModel` | `execution_model.hpp` | Mode B: the Phase 1 order book (BKM-1) |
| `CandleModel`, `QuoteModel` | `mode_a.hpp` | Mode A: fills against historical bars or L1 quotes |
| `Ledger` | `accounts.hpp` | One account: cash, positions, cost bases, P&L, fees, borrow; the fill ledger |
| `Accounts` | `accounts.hpp` | `AccountHooks` for a model: checks, reservations, borrow, equity curve, reconciliation |

Every model reports to an `AccountHooks`. The Mode A models call the hooks while they fill, so a
second fill in the same event already sees the first fill's cash. That matters for the fill-time
cash cap (OP-1). `BookModel` reports after each engine operation. Owners without an account, such
as the synthetic agents of Phase 2, are neither checked nor tracked.

## 2. Running a backtest in C++

```cpp
CandleModel model(registry, CandleConfig{.slippage = {SlippageKind::FixedBps, Rate{500}}});
Accounts accounts(model);                    // registers itself as the model's hooks
Journal journal;
Simulator sim(model, journal);
const OwnerId me = sim.add_strategy(strategy);
accounts.open(me, AccountConfig{.initial_cash = Money{10'000 * 100'000'000},
                                .fees = {.maker = Rate{200}, .taker = Rate{1000}}});
sim.attach_accounts(accounts);
sim.attach_source(replay);                   // Phase 3 data, or schedule_market_data(...)
sim.run();
sim.finish();                                // OP-3, then ACC-10 and ACC-11
const Ledger& l = *accounts.ledger(me);      // cash(), position(i), realised_pnl(), ...
auto curve = accounts.equity_curve(me);      // one EquityPoint per bar timestamp
```

Strategies see their own account through `Context::account()`.

## 3. Timing: why there is no look-ahead

A strategy sees bar *t* at `close_time(t)` (TIME-2, TIME-3). An order it sends then has
`sent_ts = close_time(t)` and arrives `latency` later. The CandleModel makes the order eligible on bar
*b* only if `open_time(b) ≥ sent_ts` (CND-1). The first such bar is *t+1*, so a market order fills at
`open[t+1]`, a price that did not exist when the strategy decided. An order sent mid-bar, from a
timer, waits for the bar after next.

The QuoteModel fills at the first quote at or after arrival (QTE-1). Exchange actions sort before
market data at the same timestamp (EVT-1), so an order arriving exactly at a quote's time sees that
quote, and never an earlier one.

Two tests pin this down (`engine/tests/backtest_test.cpp`):

- **Disjoint prices.** Each bar's prices lie in their own range, so a fill price identifies its
  bar. Over 200 bars of random market and limit orders, every fill comes from a bar after the
  decision bar.
- **Next-close oracle.** Every bar opens at the same price, and a strategy is handed `close[t+1]`
  at bar *t*. It buys only before rising bars and sells one bar later. With fills at opens it
  realises exactly 0. An engine that filled at closes would let it profit.

## 4. Fill rules in one table

| | CandleModel (bar *b*) | QuoteModel (quote *q*) |
|---|---|---|
| MARKET buy | `open[b]` plus slippage, rounded up to the tick | `ask`, capped at `ask_qty` |
| LIMIT buy *L* | if `low < L`: `min(open, L)` (strict) | if `ask ≤ L`: `ask`, capped at `ask_qty` |
| GTC remainder | keeps working on later bars | keeps working on later quotes |
| IOC | first eligible bar only, then `IOC_REMAINDER` | first quote only, then `IOC_REMAINDER` |
| FOK | full size on the first eligible bar, or `FOK_UNFILLABLE` | full size on the first quote, or `FOK_UNFILLABLE` |
| Size limit | optional `participation × volume`, per owner, bar and instrument, in `seq` order | displayed size, per owner, in `seq` order |
| Liquidity | taker | taker |
| Mark (ACC-6) | last close | `floor((bid + ask) / 2)` |

Sells mirror buys: a limit sell needs `high > L` and fills at `max(open, L)`; slippage is rounded
down. MARKET remainders are always cancelled `MARKET_REMAINDER` (ORD-6). Mode A trades have no
resting order: `resting` and `resting_owner` are 0.

## 5. Accounting

The ledger stores each position's signed **cost basis**, not its average price.

- Buying adds the notional to the basis.
- Reducing releases `trunc(basis × q / |position|)`. Closing the position releases everything that
  is left.
- A flip is a close followed by an open at the same price, inside one fill.
- Short-sale proceeds go to cash.
- Fees are `max(ceil(notional × rate / 1e6), min_fee)`, maker or taker.
- Borrow cost is charged at each UTC midnight on the short market value.

Everything is an integer number of Money units (1e-8 of the quote currency), so reconciliation is
exact:

```text
ACC-10   cash + Σ cost_basis == initial_cash + realised − fees − borrow
         equity == initial_cash + realised + unrealised − fees − borrow
ACC-11   applying the fill ledger (fills and borrow charges) from scratch reproduces cash,
         positions, bases, realised P&L, fees and borrow costs
```

- **Checked when:** Debug builds check ACC-10 after every fill. `Accounts::finish` checks ACC-10
  and ACC-11 for every account at the end of every run; a failure is a bug and aborts.
- **Pre-trade checks (ACC-9):** run in this order: cash (net of reservations) → position limit →
  shorts → gross leverage. The last three use the projected position, which includes the account's
  open orders.
- **Reservations (ACC-7):** an open LIMIT buy reserves `limit × remaining` plus the taker fee. The
  amount is recomputed on every update, so a partial fill releases exactly its share.

## 6. Tests

- **Model rules:** `mode_a_test.cpp` has a golden case per rule, including the boundaries
  `low == limit` (no fill) and `ask == limit` (fill), slippage rounding, the volume cap, and
  amend/cancel.
- **Accounting:** `accounts_test.cpp` walks open, add, reduce, close, flip and cover a short, plus
  basis truncation, fee rounding and minimums, each pre-trade rejection and the order of the checks,
  reservations, OP-1, borrow accrual, equity snapshots, and Book maker/taker fees.
- **End to end:** `backtest_test.cpp` has:
  - a scripted backtest ("buy 10 at bar 3, sell 4 at bar 7") with hand-computed cash, basis,
    realised and unrealised P&L and equity curve;
  - OP-3 cancellation at the end of a run;
  - both look-ahead tests;
  - **1000 seeded randomised runs** across both models, each with 2 accounts and random fees,
    limits, leverage, shorts, borrow, slippage, caps, latency, cancels and amends. Every run must
    reconcile. Together the runs make over 20 000 fills and over 1 000 rejections.
- **Mutation check:** leaking a single Money unit on some fills makes these tests fail.
