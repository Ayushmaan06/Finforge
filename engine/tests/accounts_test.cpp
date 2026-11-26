// Golden cases for the ledger and the account hooks (contracts.md §8 FEE-1, §9 ACC-1…11).
// Instrument SIM has tick 1 and lot 1, so money_per_tick_lot = 1e8: one lot at one tick is one
// currency unit, U = 100 000 000 Money. Every expected value is computed by hand in the comments.

#include "finforge/accounts.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "finforge/mode_a.hpp"
#include "test_util.hpp"

namespace finforge {
namespace {

using test::kInst;
constexpr std::int64_t U = 100'000'000;  // Money per currency unit for SIM
constexpr Rate kTenBps{1000};

Money m(std::int64_t units_e8) { return Money{units_e8}; }

class LedgerTest : public ::testing::Test {
  protected:
    InstrumentRegistry reg = test::make_registry();
    Ledger ledger{reg, AccountConfig{.initial_cash = Money{10'000 * U},
                                     .fees = {.maker = Rate{0}, .taker = kTenBps}}};
    std::int64_t trade = 0;

    void fill(Side side, std::int64_t price, std::int64_t qty) {
        const Money n = ledger.notional(kInst, Price{price}, Qty{qty});
        ledger.apply(LedgerFill{
            Timestamp{trade}, OrderId{1}, TradeId{static_cast<std::uint64_t>(++trade)}, kInst, side,
            Price{price}, Qty{qty}, ledger.fee(n, Liquidity::Taker), Liquidity::Taker});
        ASSERT_TRUE(ledger.reconciles());                   // ACC-10, after every fill
        ASSERT_TRUE(ledger.replayed().same_state(ledger));  // ACC-11
    }
    void expect(std::int64_t cash, std::int64_t qty, std::int64_t basis, std::int64_t realised,
                std::int64_t fees) {
        EXPECT_EQ(ledger.cash(), m(cash));
        EXPECT_EQ(ledger.position(kInst).qty, Qty{qty});
        EXPECT_EQ(ledger.position(kInst).cost_basis, m(basis));
        EXPECT_EQ(ledger.realised_pnl(), m(realised));
        EXPECT_EQ(ledger.total_fees(), m(fees));
    }
};

TEST_F(LedgerTest, OpenAddReduceCloseFlipCover) {
    // Open long: buy 10 @ 100. Notional 1000 U, fee ceil(1000 U × 10 bps) = 1 U.
    // cash 10000 − 1000 − 1 = 8999 U.
    fill(Side::Buy, 100, 10);
    expect(8'999 * U, 10, 1'000 * U, 0, 1 * U);

    // Add: buy 10 @ 110. Notional 1100 U, fee 1.1 U. cash 8999 − 1100 − 1.1 = 7897.9 U.
    fill(Side::Buy, 110, 10);
    expect(789'790'000'000, 20, 2'100 * U, 0, 210'000'000);
    // ACC-6 at mark 115: value 20 × 115 = 2300 U, unrealised 200 U, equity 10197.9 U.
    const std::vector<Price> marks{Price{115}, Price{0}};
    EXPECT_EQ(ledger.equity(marks), m(1'019'790'000'000));
    EXPECT_EQ(ledger.unrealised_pnl(marks), m(200 * U));
    EXPECT_TRUE(ledger.reconciles(marks));

    // Reduce: sell 5 @ 120. Release trunc(2100 × 5 / 20) = 525 U; realised 600 − 525 = 75 U.
    // cash 7897.9 + 600 − 0.6 = 8497.3 U.
    fill(Side::Sell, 120, 5);
    expect(849'730'000'000, 15, 1'575 * U, 75 * U, 270'000'000);

    // Close: sell 15 @ 90. Release all 1575 U; realised 75 + 1350 − 1575 = −150 U.
    // cash 8497.3 + 1350 − 1.35 = 9845.95 U.
    fill(Side::Sell, 90, 15);
    expect(984'595'000'000, 0, 0, -150 * U, 405'000'000);

    // Flip long → short in one fill (ACC-4). First buy 10 @ 100 (fee 1 U, cash 8844.95 U), then
    // sell 25 @ 104: close 10 (proceeds 1040, release 1000, realised +40 → −110 U), open short 15
    // (basis −1560 U, proceeds credited, ACC-5). Fee on the whole fill: 2600 U × 10 bps = 2.6 U.
    // cash 8844.95 + 1040 + 1560 − 2.6 = 11442.35 U.
    fill(Side::Buy, 100, 10);
    fill(Side::Sell, 104, 25);
    expect(1'144'235'000'000, -15, -1'560 * U, -110 * U, 765'000'000);

    // Cover the short: buy 15 @ 100. Proceeds −1500, release −1560: realised +60 → −50 U.
    // cash 11442.35 − 1500 − 1.5 = 9940.85 U. Check: 10000 − 50 − 9.15 = 9940.85.
    fill(Side::Buy, 100, 15);
    expect(994'085'000'000, 0, 0, -50 * U, 915'000'000);
}

TEST_F(LedgerTest, OpenAndCloseShort) {
    // Sell 10 @ 50 short: cash 10000 + 500 − 0.5 = 10499.5 U, basis −500 U.
    fill(Side::Sell, 50, 10);
    expect(1'049'950'000'000, -10, -500 * U, 0, 50'000'000);
    // Buy back 10 @ 40: realised −400 − (−500) = +100 U. cash 10499.5 − 400 − 0.4 = 10099.1 U.
    fill(Side::Buy, 40, 10);
    expect(1'009'910'000'000, 0, 0, 100 * U, 90'000'000);
}

TEST(Ledger, ACC3_BasisReleaseTruncatesTowardZeroAndClosingReleasesTheRest) {
    const InstrumentRegistry reg = test::make_registry();
    Ledger l(reg, AccountConfig{.initial_cash = Money{10'000 * U}, .allow_short = true});
    auto f = [&](Side s, std::int64_t p, std::int64_t q) {
        l.apply(LedgerFill{Timestamp{}, OrderId{1}, TradeId{1}, kInst, s, Price{p}, Qty{q},
                           Money{0}, Liquidity::Taker});
    };
    f(Side::Buy, 100, 1);
    f(Side::Buy, 101, 2);  // basis 302 U over 3 lots
    f(Side::Sell, 101, 1);
    // trunc(30 200 000 000 / 3) = 10 066 666 666; realised 10 100 000 000 − that = 33 333 334.
    EXPECT_EQ(l.position(kInst).cost_basis, Money{20'133'333'334});
    EXPECT_EQ(l.realised_pnl(), Money{33'333'334});
    f(Side::Sell, 101, 2);  // the rest: exactly 1 U of profit in total (bought 302, sold 303)
    EXPECT_EQ(l.position(kInst).cost_basis, Money{0});
    EXPECT_EQ(l.realised_pnl(), Money{1 * U});

    // Shorts truncate toward zero too: basis −302 U, cover 1 → release −10 066 666 666.
    f(Side::Sell, 100, 1);
    f(Side::Sell, 101, 2);
    f(Side::Buy, 101, 1);
    EXPECT_EQ(l.position(kInst).cost_basis, Money{-20'133'333'334});
    EXPECT_EQ(l.realised_pnl(), Money{1 * U - 33'333'334});
    EXPECT_TRUE(l.reconciles());
    EXPECT_TRUE(l.replayed().same_state(l));
}

TEST(Ledger, FEE1_RoundsUpAppliesMinimumAndSeparatesMakerTaker) {
    InstrumentRegistry reg;  // tick 0.01 × lot 0.000001 / 1e-8 = 1 Money per tick-lot
    reg.add(
        {.symbol = "TINY", .quote_currency = "USD", .tick_size = "0.01", .lot_size = "0.000001"});
    const Ledger l(reg, AccountConfig{.fees = {.maker = Rate{200}, .taker = kTenBps}});
    EXPECT_EQ(l.notional(kInst, Price{1}, Qty{1}), Money{1});
    EXPECT_EQ(l.fee(Money{1}, Liquidity::Taker), Money{1});       // ceil(0.001): at least 1 unit
    EXPECT_EQ(l.fee(Money{1000}, Liquidity::Taker), Money{1});    // exactly 1
    EXPECT_EQ(l.fee(Money{1001}, Liquidity::Taker), Money{2});    // ceil(1.001)
    EXPECT_EQ(l.fee(Money{10'000}, Liquidity::Maker), Money{2});  // 2 bps
    EXPECT_EQ(l.fee(Money{0}, Liquidity::Taker), Money{0});

    const InstrumentRegistry sim = test::make_registry();
    const Ledger with_min(sim, AccountConfig{.fees = {.taker = kTenBps, .min_fee = Money{5 * U}}});
    EXPECT_EQ(with_min.fee(Money{1'000 * U}, Liquidity::Taker), Money{5 * U});    // max(1 U, 5 U)
    EXPECT_EQ(with_min.fee(Money{10'000 * U}, Liquidity::Taker), Money{10 * U});  // max(10 U, 5 U)
    const Ledger zero_rate(sim, AccountConfig{.fees = {.min_fee = Money{5 * U}}});
    EXPECT_EQ(zero_rate.fee(Money{1 * U}, Liquidity::Taker), Money{5 * U});
}

// --- Accounts + CandleModel ---------------------------------------------------------------------

constexpr std::int64_t kMin = 60;

Bar bar(std::int64_t i, std::int64_t o, std::int64_t h, std::int64_t l, std::int64_t c,
        std::int64_t v = 1'000'000, InstrumentId inst = kInst) {
    return Bar{inst,
               Timestamp{i * kMin},
               Timestamp{(i + 1) * kMin},
               Price{o},
               Price{h},
               Price{l},
               Price{c},
               Qty{v}};
}

NewOrder buy(OrderType type, std::int64_t price, std::int64_t qty, std::int64_t sent = kMin,
             InstrumentId inst = kInst) {
    return NewOrder{OwnerId{1},   inst,     Side::Buy,           type,           Tif::Gtc,
                    Price{price}, Qty{qty}, Timestamp{sent + 1}, Timestamp{sent}};
}
NewOrder sell(OrderType type, std::int64_t price, std::int64_t qty, std::int64_t sent = kMin) {
    NewOrder o = buy(type, price, qty, sent);
    o.side = Side::Sell;
    return o;
}

struct Fixture {
    explicit Fixture(AccountConfig c, CandleConfig cc = {})
        : model(test::make_registry(), cc), acc(model) {
        acc.open(OwnerId{1}, c);
        model.on_market_data(bar(0, 100, 100, 100, 100));  // mark 100
    }
    RejectReason submit(const NewOrder& o) { return model.order(model.submit(o))->reject_reason; }
    const Ledger& ledger() const { return *acc.ledger(OwnerId{1}); }
    CandleModel model;
    Accounts acc;
};

TEST(Accounts, ACC9_EachCheckAndTheirOrder) {
    Fixture f(AccountConfig{.initial_cash = Money{1'000 * U}, .max_position = Qty{5}});
    // Fails cash, position limit and leverage: cash is checked first.
    EXPECT_EQ(f.submit(buy(OrderType::Market, 0, 20)), RejectReason::InsufficientCash);
    // Cash is fine (600 U ≤ 1000 U), position 6 > 5.
    EXPECT_EQ(f.submit(buy(OrderType::Market, 0, 6)), RejectReason::PositionLimit);
    // No position: a sell would go short.
    EXPECT_EQ(f.submit(sell(OrderType::Market, 0, 1)), RejectReason::ShortNotAllowed);
    // The model's own checks come before the account's, even with no cash.
    NewOrder unknown = buy(OrderType::Market, 0, 20);
    unknown.instrument = InstrumentId{9};
    EXPECT_EQ(f.submit(unknown), RejectReason::UnknownInstrument);
    EXPECT_EQ(f.submit(buy(OrderType::Limit, 0, 1)), RejectReason::InvalidPrice);
    EXPECT_EQ(f.submit(buy(OrderType::Market, 0, 5)), RejectReason::None);

    // Leverage: shorts allowed, 1.0x. Short 15 at mark 100 = 1500 U gross on 1000 U equity.
    Fixture g(AccountConfig{.initial_cash = Money{1'000 * U}, .allow_short = true});
    EXPECT_EQ(g.submit(sell(OrderType::Market, 0, 15)), RejectReason::LeverageLimit);
    EXPECT_EQ(g.submit(sell(OrderType::Market, 0, 10)), RejectReason::None);  // exactly 1.0x
    // Open orders count: another short of 1 would make 11 × 100 > 1000.
    EXPECT_EQ(g.submit(sell(OrderType::Market, 0, 1)), RejectReason::LeverageLimit);
    // 2x allows it.
    Fixture h(AccountConfig{
        .initial_cash = Money{1'000 * U}, .allow_short = true, .max_leverage = Rate{2'000'000}});
    EXPECT_EQ(h.submit(sell(OrderType::Market, 0, 15)), RejectReason::None);
}

TEST(Accounts, ACC9_MarketReferenceIncludesSlippage) {
    // Mark 100 with 10% slippage: buy reference 110. 9 lots: 990 U fits 1000 U; 10 lots do not.
    Fixture f(AccountConfig{.initial_cash = Money{1'000 * U}},
              CandleConfig{.slippage = {SlippageKind::FixedBps, Rate{100'000}}});
    EXPECT_EQ(f.submit(buy(OrderType::Market, 0, 10)), RejectReason::InsufficientCash);
    EXPECT_EQ(f.submit(buy(OrderType::Market, 0, 9)), RejectReason::None);
}

TEST(Accounts, ACC7_ReservedCashIsHeldReleasedProRataAndOnCancel) {
    // 10% of 20 lots = 2 lots fill per bar.
    Fixture f(AccountConfig{.initial_cash = Money{1'000 * U}},
              CandleConfig{.participation = Rate{100'000}});
    const OrderId id = f.model.submit(buy(OrderType::Limit, 100, 6));
    EXPECT_EQ(f.ledger().reserved_cash(), Money{600 * U});
    // 1000 − 600 = 400 U free: 5 lots at 100 do not fit.
    EXPECT_EQ(f.submit(buy(OrderType::Limit, 100, 5)), RejectReason::InsufficientCash);

    f.model.on_market_data(bar(1, 101, 102, 95, 100, 20));  // trades through: 2 lots at 100
    EXPECT_EQ(f.model.order(id)->filled, Qty{2});
    EXPECT_EQ(f.ledger().cash(), Money{800 * U});
    EXPECT_EQ(f.ledger().reserved_cash(), Money{400 * U});  // 4 lots × 100 still reserved

    ASSERT_TRUE(f.model.cancel(id).ok());
    EXPECT_EQ(f.ledger().reserved_cash(), Money{0});
    EXPECT_EQ(f.submit(buy(OrderType::Limit, 100, 5)), RejectReason::None);  // 500 ≤ 800 now
    EXPECT_TRUE(f.acc.reconciles(OwnerId{1}));
}

TEST(Accounts, OP1_BuyIsCutToWhatCashPaysAtFillTime) {
    Fixture f(AccountConfig{.initial_cash = Money{1'000 * U}, .fees = {.taker = kTenBps}});
    // Checked at mark 100: 900 U + 0.9 U fee ≤ 1000 U.
    const OrderId id = f.model.submit(buy(OrderType::Market, 0, 9));
    ASSERT_EQ(f.model.order(id)->status, OrderStatus::Accepted);
    // Opens at 120: 9 lots cost 1080 U. 8 lots: 960 + 0.96 ≤ 1000. Fill 8, cancel 1.
    f.model.on_market_data(bar(1, 120, 121, 119, 120));
    const Order& o = *f.model.order(id);
    EXPECT_EQ(o.filled, Qty{8});
    EXPECT_EQ(o.cancelled, Qty{1});
    EXPECT_EQ(o.cancel_reason, CancelReason::InsufficientCashAtFill);
    EXPECT_EQ(f.ledger().cash(), Money{3'904'000'000});  // 1000 − 960 − 0.96 = 39.04 U
    EXPECT_TRUE(f.acc.reconciles(OwnerId{1}));
}

TEST(Accounts, ACC8_BorrowAccruesAtEveryUtcMidnight) {
    constexpr std::int64_t kDay = 86'400'000'000'000;
    Fixture f(AccountConfig{.initial_cash = Money{10'000 * U},
                            .allow_short = true,
                            .borrow_rate = Rate{100'000}});  // 10% a year
    f.acc.advance(Timestamp{kMin});
    f.model.submit(sell(OrderType::Market, 0, 10));
    f.model.on_market_data(bar(1, 100, 100, 100, 100));  // short 10 at 100: value −1000 U
    f.acc.advance(Timestamp{kDay - 1});
    EXPECT_EQ(f.ledger().borrow_costs(), Money{0});
    // ceil(1000 U × 0.1 / 365) = ceil(1e10 / 365) = 27 397 261 per day.
    f.acc.advance(Timestamp{kDay});
    EXPECT_EQ(f.ledger().borrow_costs(), Money{27'397'261});
    f.acc.advance(Timestamp{3 * kDay + 5});  // two more midnights
    EXPECT_EQ(f.ledger().borrow_costs(), Money{3 * 27'397'261});
    EXPECT_TRUE(f.acc.reconciles(OwnerId{1}));  // replay includes the borrow charges (ACC-11)
}

TEST(Accounts, EquitySnapshotsOncePerBarTimestampOrPerInterval) {
    InstrumentRegistry reg = test::make_registry();
    CandleModel model(reg);
    Accounts acc(model);
    acc.open(OwnerId{1}, AccountConfig{.initial_cash = Money{100 * U}});
    for (std::int64_t i = 0; i < 3; ++i) {
        for (std::uint32_t inst = 0; inst < 2; ++inst) {  // two instruments close together
            const Bar b = bar(i, 10, 10, 10, 10, 1, InstrumentId{inst});
            model.on_market_data(b);
            acc.after_market_data(b, b.close_time);
        }
    }
    ASSERT_EQ(acc.equity_curve(OwnerId{1}).size(), 3U);
    EXPECT_EQ(acc.equity_curve(OwnerId{1})[2].ts, Timestamp{3 * kMin});
    EXPECT_EQ(acc.equity_curve(OwnerId{1})[2].equity, Money{100 * U});

    CandleModel model2(reg);
    Accounts timed(model2, AccountsConfig{.equity_interval_ns = 100});
    timed.open(OwnerId{1}, AccountConfig{.initial_cash = Money{1}});
    timed.advance(Timestamp{30});
    timed.advance(Timestamp{350});  // boundaries 100, 200, 300
    ASSERT_EQ(timed.equity_curve(OwnerId{1}).size(), 3U);
    EXPECT_EQ(timed.equity_curve(OwnerId{1})[0].ts, Timestamp{100});
}

TEST(Accounts, BookModelMakerAndTakerFees) {
    BookModel model(test::make_registry());
    Accounts acc(model);
    const AccountConfig c{.initial_cash = Money{10'000 * U},
                          .fees = {.maker = Rate{0}, .taker = kTenBps},
                          .allow_short = true};
    acc.open(OwnerId{1}, c);
    acc.open(OwnerId{2}, c);
    model.submit(NewOrder{OwnerId{1}, kInst, Side::Sell, OrderType::Limit, Tif::Gtc, Price{100},
                          Qty{10}, Timestamp{1}});
    model.submit(NewOrder{OwnerId{2}, kInst, Side::Buy, OrderType::Market, Tif::Gtc, Price{0},
                          Qty{10}, Timestamp{2}});
    const Ledger& maker = *acc.ledger(OwnerId{1});
    const Ledger& taker = *acc.ledger(OwnerId{2});
    EXPECT_EQ(maker.cash(), Money{11'000 * U});  // short proceeds, no maker fee
    EXPECT_EQ(maker.total_fees(), Money{0});
    EXPECT_EQ(taker.cash(), Money{8'999 * U});  // 1000 U + 1 U taker fee
    EXPECT_EQ(taker.position(kInst).qty, Qty{10});
    EXPECT_TRUE(acc.reconciles(OwnerId{1}));
    EXPECT_TRUE(acc.reconciles(OwnerId{2}));

    // Pre-trade checks run on book orders too, including amend replacements.
    const OrderId big = model.submit(NewOrder{OwnerId{2}, kInst, Side::Buy, OrderType::Limit,
                                              Tif::Gtc, Price{50}, Qty{10}, Timestamp{3}});
    EXPECT_EQ(model.order(big)->status, OrderStatus::Accepted);  // 500 U reserved
    const ActionResult r = model.amend(big, Qty{1000}, Price{50}, Timestamp{4}, Timestamp{4});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(model.order(r.new_order)->reject_reason, RejectReason::InsufficientCash);
}

TEST(Accounts, ACC9_BookAmendIsCheckedWithoutTheOrderItReplaces) {
    BookModel model(test::make_registry());
    Accounts acc(model);
    acc.open(OwnerId{1}, AccountConfig{.initial_cash = Money{1'000 * U}});
    const OrderId id = model.submit(NewOrder{OwnerId{1}, kInst, Side::Buy, OrderType::Limit,
                                             Tif::Gtc, Price{50}, Qty{10}, Timestamp{1}});
    ASSERT_EQ(acc.ledger(OwnerId{1})->reserved_cash(), Money{500 * U});
    // 15 × 50 = 750 U fits 1000 U once the replaced order's 500 U is released.
    const ActionResult r = model.amend(id, Qty{15}, Price{50}, Timestamp{2}, Timestamp{2});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(model.order(r.new_order)->status, OrderStatus::Accepted);
    EXPECT_EQ(acc.ledger(OwnerId{1})->reserved_cash(), Money{750 * U});
    EXPECT_TRUE(acc.reconciles(OwnerId{1}));
}

}  // namespace
}  // namespace finforge
