// End-to-end Mode A backtests through the simulator (PLAN Phase 4 tasks 7, exit criteria):
// scripted strategies with hand-computed results, the look-ahead tests, and seeded randomised runs
// that must always reconcile (ACC-10, ACC-11).

#include <gtest/gtest.h>

#include <functional>
#include <map>
#include <memory>
#include <variant>
#include <vector>

#include "finforge/accounts.hpp"
#include "finforge/journal.hpp"
#include "finforge/mode_a.hpp"
#include "finforge/rng.hpp"
#include "finforge/simulator.hpp"
#include "test_util.hpp"

namespace finforge {
namespace {

using test::kInst;
constexpr std::int64_t U = 100'000'000;  // Money per currency unit for SIM (tick 1, lot 1)
constexpr std::int64_t kMin = 60'000'000'000;

Bar bar(std::int64_t i, std::int64_t o, std::int64_t h, std::int64_t l, std::int64_t c,
        std::int64_t interval = kMin, InstrumentId inst = kInst, std::int64_t v = 1'000'000) {
    return Bar{inst,
               Timestamp{i * interval},
               Timestamp{(i + 1) * interval},
               Price{o},
               Price{h},
               Price{l},
               Price{c},
               Qty{v}};
}

// A strategy built from callbacks; counts bars per instrument.
class Scripted final : public Strategy {
  public:
    std::function<void(Context&, std::int64_t bar_index, const MarketData&)> on_data;
    std::function<void(Context&, const Trade&)> fill;
    std::function<void(Context&, const OwnOrderUpdate&)> update;

    void on_market_data(Context& c, const MarketData& md) override {
        const std::int64_t i =
            std::holds_alternative<Bar>(md) ? bars_[instrument_of(md).value()]++ : -1;
        if (on_data) on_data(c, i, md);
    }
    void on_fill(Context& c, const Trade& t) override {
        if (fill) fill(c, t);
    }
    void on_order_update(Context& c, const OwnOrderUpdate& u) override {
        if (update) update(c, u);
    }

  private:
    std::map<std::uint32_t, std::int64_t> bars_;
};

OrderRequest market(Side s, std::int64_t qty, InstrumentId inst = kInst) {
    return OrderRequest{inst, s, OrderType::Market, Tif::Gtc, Price{0}, Qty{qty}};
}

// --- Scripted golden backtest -------------------------------------------------------------------

TEST(Backtest, ScriptedBuyTenAtBar3SellFourAtBar7) {
    // Bar i: open 100 + 2i, high +5, low −5, close open + 1.
    CandleModel model(test::make_registry());
    Accounts accounts(model);
    Journal journal;
    Simulator sim(model, journal);
    Scripted s;
    const OwnerId me = sim.add_strategy(s);
    accounts.open(me, AccountConfig{.initial_cash = Money{10'000 * U},
                                    .fees = {.taker = Rate{1000}}});  // 10 bps
    sim.attach_accounts(accounts);
    std::vector<Trade> fills;
    s.fill = [&](Context&, const Trade& t) { fills.push_back(t); };
    s.on_data = [&](Context& c, std::int64_t i, const MarketData&) {
        EXPECT_NE(c.account(), nullptr);
        if (i == 3) c.submit(market(Side::Buy, 10));
        if (i == 7) c.submit(market(Side::Sell, 4));
    };
    for (std::int64_t i = 0; i < 10; ++i) {
        const std::int64_t o = 100 + 2 * i;
        sim.schedule_market_data(bar(i, o, o + 5, o - 5, o + 1));
    }
    EXPECT_EQ(sim.run(), RunStatus::Completed);
    sim.finish();

    // Bought at bar 4's open (108), sold at bar 8's open (116).
    ASSERT_EQ(fills.size(), 2U);
    EXPECT_EQ(fills[0].price, Price{108});
    EXPECT_EQ(fills[0].ts, Timestamp{5 * kMin});  // known at bar 4's close
    EXPECT_EQ(fills[1].price, Price{116});

    const Ledger& l = *accounts.ledger(me);
    // Buy: 1080 U + fee 1.08 U → cash 8918.92 U, basis 1080 U.
    // Sell 4: 464 U − fee 0.464 U; release trunc(1080 × 4 / 10) = 432 U → realised 32 U.
    // cash 8918.92 + 464 − 0.464 = 9382.456 U; basis 648 U; fees 1.544 U.
    EXPECT_EQ(l.cash(), Money{938'245'600'000});
    EXPECT_EQ(l.position(kInst).qty, Qty{6});
    EXPECT_EQ(l.position(kInst).cost_basis, Money{648 * U});
    EXPECT_EQ(l.realised_pnl(), Money{32 * U});
    EXPECT_EQ(l.total_fees(), Money{154'400'000});
    // Final mark: bar 9 closes at 119 → 6 × 119 = 714 U; equity 10096.456 U; unrealised 66 U.
    const auto curve = accounts.equity_curve(me);
    ASSERT_EQ(curve.size(), 10U);  // one point per bar
    EXPECT_EQ(curve.back().equity, Money{1'009'645'600'000});
    EXPECT_EQ(curve.back().unrealised_pnl, Money{66 * U});
    // After bar 4: cash 8918.92 U + 10 × 109 = 10008.92 U.
    EXPECT_EQ(curve[4].equity, Money{1'000'892'000'000});
    EXPECT_EQ(curve[3].equity, Money{10'000 * U});
}

TEST(Backtest, OP3_OpenOrdersAreCancelledAtTheEnd) {
    CandleModel model(test::make_registry());
    Accounts accounts(model);
    Journal journal;
    Simulator sim(model, journal);
    Scripted s;
    const OwnerId me = sim.add_strategy(s);
    accounts.open(me, AccountConfig{.initial_cash = Money{10'000 * U}});
    sim.attach_accounts(accounts);
    std::vector<CancelReason> cancels;
    s.on_data = [&](Context& c, std::int64_t i, const MarketData&) {
        if (i == 0)
            c.submit(OrderRequest{kInst, Side::Buy, OrderType::Limit, Tif::Gtc, Price{1}, Qty{5}});
    };
    s.update = [&](Context& c, const OwnOrderUpdate& u) {
        if (u.update.status == OrderStatus::Cancelled) {
            cancels.push_back(u.update.cancel_reason);
            c.submit(market(Side::Buy, 1));  // dropped: the run is over
        }
    };
    sim.schedule_market_data(bar(0, 100, 100, 100, 100));
    sim.schedule_market_data(bar(1, 100, 100, 100, 100));
    sim.run();
    EXPECT_EQ(accounts.ledger(me)->reserved_cash(), Money{5 * U});
    sim.finish();
    EXPECT_EQ(cancels, std::vector<CancelReason>{CancelReason::EndOfRun});
    EXPECT_EQ(accounts.ledger(me)->reserved_cash(), Money{0});
    EXPECT_EQ(sim.queued(), 0U);
    EXPECT_THROW(sim.finish(), std::logic_error);
}

// --- Look-ahead (exit criterion 2) --------------------------------------------------------------

TEST(Backtest, LookAhead_OrderSentAtBarCloseNeverFillsAtThatBarsPrices) {
    // Bar i's prices all lie in [1000 i + 100, 1000 i + 900], so a fill price names its bar.
    CandleModel model(test::make_registry());
    Accounts accounts(model);
    Journal journal;
    Simulator sim(model, journal);
    Scripted s;
    const OwnerId me = sim.add_strategy(s);
    accounts.open(me, AccountConfig{.initial_cash = Money{1'000'000'000 * U},
                                    .allow_short = true,
                                    .max_leverage = Rate{1'000'000'000}});
    sim.attach_accounts(accounts);
    Pcg32 rng(7, 1);
    std::map<std::uint64_t, std::int64_t> sent_on;  // client ref -> bar index
    std::map<std::uint64_t, std::uint64_t> ref_of;  // order id -> client ref
    s.update = [&](Context&, const OwnOrderUpdate& u) {
        ref_of[u.update.order.value()] = u.client_ref;
    };
    s.on_data = [&](Context& c, std::int64_t i, const MarketData& md) {
        const Bar& b = std::get<Bar>(md);
        for (int k = 0; k < 3; ++k) {
            const Side side = rng.chance(50) ? Side::Buy : Side::Sell;
            OrderRequest r = market(side, 1 + rng.below(5));
            if (rng.chance(50)) {  // a limit around this bar's close, often marketable
                r.type = OrderType::Limit;
                r.tif = rng.chance(50) ? Tif::Gtc : Tif::Ioc;
                r.price = Price{b.close.value() + rng.range(-400, 400) +
                                (side == Side::Buy ? 1000 : -1000)};
                if (r.price <= Price{0}) r.price = Price{1};
            }
            sent_on[c.submit(r)] = i;
        }
    };
    s.fill = [&](Context&, const Trade& t) {
        const std::int64_t fill_bar = t.price.value() / 1000;
        const std::int64_t decided = sent_on.at(ref_of.at(t.aggressor.value()));
        EXPECT_GT(fill_bar, decided) << "filled at a price of the bar the order was sent on";
    };
    for (std::int64_t i = 0; i < 200; ++i) {
        const std::int64_t base = 1000 * i;
        sim.schedule_market_data(bar(i, base + 100 + rng.range(0, 800), base + 900, base + 100,
                                     base + 100 + rng.range(0, 800)));
    }
    sim.run();
    sim.finish();
    EXPECT_GT(model.trade_count(), 300U);
}

TEST(Backtest, LookAhead_KnowingTheNextCloseEarnsNothing) {
    // Every bar opens at 1000; closes are random. An oracle that knows close[t+1] at bar t buys
    // only before rising bars and sells one bar later. Fills are at opens, so it gains exactly 0.
    constexpr std::int64_t kBars = 300;
    Pcg32 rng(11, 3);
    std::vector<std::int64_t> close(kBars);
    for (auto& c : close) c = 900 + rng.range(0, 200);

    CandleModel model(test::make_registry());
    Accounts accounts(model);
    Journal journal;
    Simulator sim(model, journal);
    Scripted s;
    const OwnerId me = sim.add_strategy(s);
    accounts.open(me, AccountConfig{.initial_cash = Money{100'000 * U}});
    sim.attach_accounts(accounts);
    std::int64_t trades = 0;
    s.fill = [&](Context&, const Trade&) { ++trades; };
    s.on_data = [&](Context& c, std::int64_t t, const MarketData&) {
        const Qty held = c.account()->position(kInst).qty;
        if (held > Qty{0}) c.submit(market(Side::Sell, held.value()));
        if (t + 1 < kBars && close[t + 1] > close[t]) c.submit(market(Side::Buy, 10));  // peeks
    };
    for (std::int64_t i = 0; i < kBars; ++i) {
        sim.schedule_market_data(bar(i, 1000, 1100, 800, close[i]));
    }
    sim.run();
    sim.finish();
    const Ledger& l = *accounts.ledger(me);
    EXPECT_GT(trades, 100);
    EXPECT_EQ(l.realised_pnl(), Money{0});
    EXPECT_EQ(l.cash(), Money{100'000 * U});  // no fees configured
}

// --- Randomised runs (exit criterion 3) ---------------------------------------------------------

struct RunStats {
    std::uint64_t fills = 0;
    std::uint64_t rejects = 0;
};

RunStats random_run(std::uint64_t seed) {
    SplitMix64 seeds(seed);
    Pcg32 rng(seeds.next(), seeds.next());
    const bool quotes = seed % 2 == 1;
    std::unique_ptr<WorkingOrderModel> model;
    if (quotes) {
        model = std::make_unique<QuoteModel>(test::make_registry());
    } else {
        CandleConfig cc;
        if (rng.chance(50)) cc.participation = Rate{rng.range(1, 1'000'000)};
        if (rng.chance(50)) {
            cc.slippage = {rng.chance(50) ? SlippageKind::FixedBps : SlippageKind::HalfSpread,
                           Rate{rng.range(0, 50'000)}};
        }
        model = std::make_unique<CandleModel>(test::make_registry(), cc);
    }
    Accounts accounts(*model,
                      AccountsConfig{.equity_interval_ns = rng.chance(30) ? 3'600'000'000'000 : 0});
    Journal journal;
    Simulator sim(*model, journal, SimConfig{.latency_ns = rng.range(1, 1000)});
    Scripted a;
    Scripted b;
    RunStats stats;
    for (Scripted* s : {&a, &b}) {
        const OwnerId owner = sim.add_strategy(*s);
        AccountConfig c;
        c.initial_cash = Money{rng.range(100, 100'000) * U};
        c.fees = {Rate{rng.range(0, 3000)}, Rate{rng.range(0, 3000)},
                  Money{rng.chance(30) ? rng.range(0, 2 * U) : 0}};
        c.allow_short = rng.chance(50);
        c.max_position = Qty{rng.chance(30) ? rng.range(1, 100) : 0};
        c.max_leverage = Rate{rng.range(500'000, 3'000'000)};
        c.borrow_rate = Rate{rng.chance(50) ? rng.range(0, 500'000) : 0};
        accounts.open(owner, c);
        s->on_data = [&rng](Context& ctx, std::int64_t, const MarketData& md) {
            const InstrumentId inst = instrument_of(md);
            const std::int64_t px = std::holds_alternative<Bar>(md)
                                        ? std::get<Bar>(md).close.value()
                                        : std::get<Quote>(md).bid.value();
            for (std::uint32_t k = rng.below(3); k > 0; --k) {
                OrderRequest r{inst,
                               rng.chance(50) ? Side::Buy : Side::Sell,
                               OrderType::Market,
                               static_cast<Tif>(rng.below(3)),
                               Price{0},
                               Qty{rng.range(1, 30)}};
                if (rng.chance(60)) {
                    r.type = OrderType::Limit;
                    r.price = Price{std::max<std::int64_t>(1, px + rng.range(-20, 20))};
                }
                ctx.submit(r);
            }
            if (rng.chance(20)) ctx.cancel(OrderId{1 + rng.below(400)});  // often not ours: refused
            if (rng.chance(10)) {
                ctx.amend(OrderId{1 + rng.below(400)}, Qty{rng.range(1, 30)},
                          Price{std::max<std::int64_t>(1, px + rng.range(-20, 20))});
            }
        };
        s->fill = [&stats](Context& ctx, const Trade&) {
            ++stats.fills;
            ASSERT_TRUE(ctx.account()->reconciles());  // ACC-10 after every fill (all builds)
        };
        s->update = [&stats](Context&, const OwnOrderUpdate& u) {
            if (u.update.status == OrderStatus::Rejected) ++stats.rejects;
        };
    }
    sim.attach_accounts(accounts);

    // Two instruments, 6-hour steps, so runs cross several UTC midnights (borrow).
    constexpr std::int64_t kStep = 6 * 3'600'000'000'000;
    std::int64_t p[2] = {100, 120};
    for (std::int64_t i = 0; i < 40; ++i) {
        for (std::uint32_t inst = 0; inst < 2; ++inst) {
            std::int64_t& c = p[inst];
            const std::int64_t o = std::max<std::int64_t>(5, c + rng.range(-3, 3));
            c = std::max<std::int64_t>(5, o + rng.range(-6, 6));
            if (quotes) {
                const std::int64_t bid = c;
                sim.schedule_market_data(Quote{
                    InstrumentId{inst}, Timestamp{(i + 1) * kStep}, Price{bid},
                    Qty{rng.range(0, 40)}, Price{bid + rng.range(1, 3)}, Qty{rng.range(0, 40)}});
            } else {
                sim.schedule_market_data(
                    bar(i, o, std::max(o, c) + rng.range(0, 4),
                        std::max<std::int64_t>(1, std::min(o, c) - rng.range(0, 4)), c, kStep,
                        InstrumentId{inst}, rng.range(0, 200)));
            }
        }
    }
    sim.run();
    sim.finish();  // OP-3, then ACC-10 and ACC-11 (FINFORGE_CHECK) for both accounts
    EXPECT_TRUE(accounts.reconciles(OwnerId{1}));
    EXPECT_TRUE(accounts.reconciles(OwnerId{2}));
    return stats;
}

TEST(Backtest, RandomisedRunsAlwaysReconcile) {
    RunStats total;
    for (std::uint64_t seed = 0; seed < 1000; ++seed) {
        SCOPED_TRACE(seed);
        const RunStats s = random_run(seed);
        total.fills += s.fills;
        total.rejects += s.rejects;
        if (::testing::Test::HasFatalFailure()) return;
    }
    // The runs exercise both fills and every kind of rejection path.
    EXPECT_GT(total.fills, 20'000U);
    EXPECT_GT(total.rejects, 1'000U);
}

TEST(Backtest, RandomisedRunIsDeterministic) {
    const RunStats a = random_run(42);
    const RunStats b = random_run(42);
    EXPECT_EQ(a.fills, b.fills);
    EXPECT_EQ(a.rejects, b.rejects);
}

}  // namespace
}  // namespace finforge
