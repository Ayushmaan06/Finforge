// Golden cases for the Mode A execution models (contracts.md §7.1, §7.2, §8): one test per rule,
// with the boundary cases. Every expected number is worked out by hand in the comments.

#include "finforge/mode_a.hpp"

#include <gtest/gtest.h>

#include <variant>
#include <vector>

#include "test_util.hpp"

namespace finforge {
namespace {

using test::kInst;
constexpr std::int64_t kMin = 60;  // bar interval in the tests (ns; units do not matter here)

Bar bar(std::int64_t index, std::int64_t o, std::int64_t h, std::int64_t l, std::int64_t c,
        std::int64_t volume = 1'000'000) {
    return Bar{kInst,
               Timestamp{index * kMin},
               Timestamp{(index + 1) * kMin},
               Price{o},
               Price{h},
               Price{l},
               Price{c},
               Qty{volume}};
}

// An order sent at `sent` (the strategy's clock) and arriving 1 ns later.
NewOrder order(Side side, OrderType type, Tif tif, std::int64_t price, std::int64_t qty,
               std::int64_t sent, OwnerId owner = OwnerId{1}) {
    return NewOrder{
        owner,          kInst, side, type, tif, Price{price}, Qty{qty}, Timestamp{sent + 1},
        Timestamp{sent}};
}
NewOrder market(Side side, std::int64_t qty, std::int64_t sent, OwnerId owner = OwnerId{1}) {
    return order(side, OrderType::Market, Tif::Gtc, 0, qty, sent, owner);
}
NewOrder limit(Side side, std::int64_t price, std::int64_t qty, std::int64_t sent,
               Tif tif = Tif::Gtc, OwnerId owner = OwnerId{1}) {
    return order(side, OrderType::Limit, tif, price, qty, sent, owner);
}

std::vector<Trade> trades(const ExecutionModel& m) { return test::trades_of(m.events()); }

// --- CandleModel --------------------------------------------------------------------------------

TEST(Candle, CND1_SentAtCloseFillsOnNextBarSentMidBarWaitsOneMore) {
    CandleModel m(test::make_registry());
    const OrderId at_close = m.submit(market(Side::Buy, 1, 1 * kMin));      // sent at bar 0's close
    const OrderId mid_bar = m.submit(market(Side::Buy, 1, 1 * kMin + 30));  // sent during bar 1

    m.on_market_data(bar(1, 100, 105, 95, 102));  // opens at 60: only the first is eligible
    auto t = trades(m);
    ASSERT_EQ(t.size(), 1U);
    EXPECT_EQ(t[0].aggressor, at_close);
    EXPECT_EQ(t[0].price, Price{100});  // CND-2: the open
    EXPECT_EQ(m.order(mid_bar)->status, OrderStatus::Accepted);

    m.on_market_data(bar(2, 103, 104, 101, 101));  // opens at 120 >= 90
    t = trades(m);
    ASSERT_EQ(t.size(), 1U);
    EXPECT_EQ(t[0].aggressor, mid_bar);
    EXPECT_EQ(t[0].price, Price{103});
}

TEST(Candle, CND2_MarketFillsFullQtyAtOpenAsTaker) {
    CandleModel m(test::make_registry());
    const OrderId id = m.submit(market(Side::Sell, 7, 0));
    m.on_market_data(bar(0, 100, 101, 99, 100));
    const auto t = trades(m);
    ASSERT_EQ(t.size(), 1U);
    EXPECT_EQ(t[0], (Trade{TradeId{1}, kInst, Price{100}, Qty{7}, id, OrderId{0}, OwnerId{1},
                           OwnerId{0}, Side::Sell, Timestamp{kMin}}));
    EXPECT_EQ(m.order(id)->status, OrderStatus::Filled);
    EXPECT_EQ(m.open_order_count(), 0U);
}

TEST(Candle, CND3_LimitNeedsStrictTradeThrough) {
    CandleModel m(test::make_registry());
    const OrderId touch = m.submit(limit(Side::Buy, 95, 1, 0));         // low == limit: no fill
    const OrderId sell_touch = m.submit(limit(Side::Sell, 105, 1, 0));  // high == limit: no fill
    m.on_market_data(bar(0, 100, 105, 95, 100));
    EXPECT_TRUE(trades(m).empty());
    EXPECT_EQ(m.order(touch)->status, OrderStatus::Accepted);
    EXPECT_EQ(m.order(sell_touch)->status, OrderStatus::Accepted);

    // Next bar trades through both: buy at min(open, limit), sell at max(open, limit).
    m.on_market_data(bar(1, 100, 106, 94, 100));
    auto t = trades(m);
    ASSERT_EQ(t.size(), 2U);
    EXPECT_EQ(t[0].price, Price{95});   // min(100, 95)
    EXPECT_EQ(t[1].price, Price{105});  // max(100, 105)
}

TEST(Candle, CND3_GapThroughTheLimitFillsAtTheOpen) {
    CandleModel m(test::make_registry());
    m.submit(limit(Side::Buy, 95, 1, 0));
    m.submit(limit(Side::Sell, 105, 1, 0));
    m.on_market_data(bar(0, 90, 92, 88, 91));  // gaps below the buy limit
    ASSERT_EQ(trades(m).size(), 1U);
    EXPECT_EQ(trades(m)[0].price, Price{90});      // min(90, 95)
    m.on_market_data(bar(1, 110, 112, 108, 111));  // gaps above the sell limit
    ASSERT_EQ(trades(m).size(), 1U);
    EXPECT_EQ(trades(m)[0].price, Price{110});  // max(110, 105)
}

TEST(Candle, CND4_TimeInForce) {
    CandleModel m(test::make_registry());
    const OrderId gtc = m.submit(limit(Side::Buy, 90, 2, 0));
    const OrderId ioc = m.submit(limit(Side::Buy, 90, 2, 0, Tif::Ioc));
    const OrderId fok_miss = m.submit(limit(Side::Buy, 90, 2, 0, Tif::Fok));
    m.on_market_data(bar(0, 100, 101, 95, 100));             // does not trade through 90
    EXPECT_EQ(m.order(gtc)->status, OrderStatus::Accepted);  // keeps working
    EXPECT_EQ(m.order(ioc)->cancel_reason, CancelReason::IocRemainder);
    EXPECT_EQ(m.order(fok_miss)->cancel_reason, CancelReason::FokUnfillable);
    EXPECT_EQ(m.order(fok_miss)->filled, Qty{0});

    const OrderId fok_hit = m.submit(limit(Side::Buy, 90, 2, kMin, Tif::Fok));
    m.on_market_data(bar(1, 92, 93, 85, 88));
    EXPECT_EQ(m.order(gtc)->status, OrderStatus::Filled);  // GTC filled on a later bar
    EXPECT_EQ(m.order(fok_hit)->status, OrderStatus::Filled);
}

TEST(Candle, CND5_VolumeCapSharedPerOwnerInSeqOrder) {
    // 10% of 25 lots = floor(2.5) = 2 lots per owner on this bar.
    CandleModel m(test::make_registry(), CandleConfig{.participation = Rate{100'000}});
    const OrderId a = m.submit(limit(Side::Buy, 200, 1, 0));
    const OrderId b = m.submit(limit(Side::Buy, 200, 3, 0));
    const OrderId mkt = m.submit(market(Side::Buy, 5, 0));
    const OrderId other = m.submit(market(Side::Buy, 5, 0, OwnerId{2}));
    const OrderId fok = m.submit(limit(Side::Buy, 200, 3, 0, Tif::Fok, OwnerId{3}));
    m.on_market_data(bar(0, 100, 101, 99, 100, 25));
    EXPECT_EQ(m.order(a)->filled, Qty{1});
    EXPECT_EQ(m.order(b)->filled, Qty{1});                        // only 1 of owner 1's 2 lots left
    EXPECT_EQ(m.order(b)->status, OrderStatus::PartiallyFilled);  // GTC remainder keeps working
    EXPECT_EQ(m.order(mkt)->filled, Qty{0});  // budget used up: remainder cancelled
    EXPECT_EQ(m.order(mkt)->cancel_reason, CancelReason::MarketRemainder);
    EXPECT_EQ(m.order(other)->filled, Qty{2});  // owner 2 has its own budget
    EXPECT_EQ(m.order(other)->cancel_reason, CancelReason::MarketRemainder);
    EXPECT_EQ(m.order(fok)->cancel_reason, CancelReason::FokUnfillable);  // 2 < 3

    m.on_market_data(bar(1, 100, 101, 99, 100, 25));
    EXPECT_EQ(m.order(b)->status, OrderStatus::Filled);  // the remaining 2 lots
}

TEST(Candle, SLP1_SlippageIsAdverseAndRoundsAgainstTheOrder) {
    // fixed 10 bps: buy 1001 × 1.001 = 1002.001 → 1003; sell 1001 × 0.999 = 999.999 → 999.
    const Slippage fixed{SlippageKind::FixedBps, Rate{1000}};
    EXPECT_EQ(fixed.apply(Price{1001}, Side::Buy), Price{1003});
    EXPECT_EQ(fixed.apply(Price{1001}, Side::Sell), Price{999});
    EXPECT_EQ(fixed.apply(Price{1000}, Side::Buy), Price{1001});  // exact: 1001.000
    // half spread of 30 bps: buy 1000 × 1.0015 = 1001.5 → 1002; sell 998.5 → 998.
    const Slippage half{SlippageKind::HalfSpread, Rate{3000}};
    EXPECT_EQ(half.apply(Price{1000}, Side::Buy), Price{1002});
    EXPECT_EQ(half.apply(Price{1000}, Side::Sell), Price{998});
    EXPECT_EQ(fixed.apply(Price{1}, Side::Sell), Price{1});  // never below one tick

    // SLP-2: market fills slip, limit fills do not.
    CandleModel m(test::make_registry(), CandleConfig{.slippage = fixed});
    m.submit(market(Side::Buy, 1, 0));
    m.submit(limit(Side::Buy, 1005, 1, 0));
    m.on_market_data(bar(0, 1001, 1010, 990, 1000));
    const auto t = trades(m);
    ASSERT_EQ(t.size(), 2U);
    EXPECT_EQ(t[0].price, Price{1003});
    EXPECT_EQ(t[1].price, Price{1001});  // min(open, limit), no slippage
}

TEST(Candle, MarkIsLastCloseAndOtherDataIsIgnored) {
    CandleModel m(test::make_registry());
    EXPECT_FALSE(m.mark(kInst));
    m.submit(market(Side::Buy, 1, 0));
    m.on_market_data(Quote{kInst, Timestamp{30}, Price{90}, Qty{5}, Price{91}, Qty{5}});
    EXPECT_TRUE(m.events().empty());
    EXPECT_FALSE(m.mark(kInst));
    m.on_market_data(bar(0, 100, 101, 99, 97));
    EXPECT_EQ(m.mark(kInst), Price{97});
}

TEST(Candle, AmendReduceInPlaceOrReplaceWithNewSentTime) {
    CandleModel m(test::make_registry());
    const OrderId id = m.submit(limit(Side::Buy, 90, 5, 0));
    ASSERT_TRUE(m.amend(id, Qty{3}, Price{90}, Timestamp{10}, Timestamp{9}).ok());
    ASSERT_EQ(m.events().size(), 1U);
    EXPECT_EQ(std::get<OrderAmended>(m.events()[0]), (OrderAmended{id, Qty{3}, Qty{3}}));

    // Price change: cancel-and-replace; the replacement was sent at 90, so bar 1 (opens at 60) is
    // too early for it.
    const ActionResult r = m.amend(id, Qty{3}, Price{99}, Timestamp{91}, Timestamp{90});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(m.order(id)->cancel_reason, CancelReason::User);
    EXPECT_EQ(m.order(r.new_order)->sent_ts, Timestamp{90});
    m.on_market_data(bar(1, 95, 96, 80, 90));
    EXPECT_TRUE(trades(m).empty());
    m.on_market_data(bar(2, 95, 96, 80, 90));
    EXPECT_EQ(m.order(r.new_order)->status, OrderStatus::Filled);

    EXPECT_EQ(m.amend(id, Qty{1}, Price{90}, Timestamp{0}, Timestamp{0}).reason,
              RejectReason::AlreadyTerminal);
    EXPECT_EQ(m.cancel(OrderId{99}).reason, RejectReason::UnknownOrder);
}

TEST(Candle, ValidationAndCancelAll) {
    CandleModel m(test::make_registry());
    EXPECT_EQ(m.order(m.submit(limit(Side::Buy, 0, 1, 0)))->reject_reason,
              RejectReason::InvalidPrice);
    EXPECT_EQ(
        m.order(m.submit(order(Side::Buy, OrderType::Market, Tif::Gtc, 5, 1, 0)))->reject_reason,
        RejectReason::InvalidPrice);  // ORD-6
    EXPECT_EQ(m.order(m.submit(limit(Side::Buy, 10, 0, 0)))->reject_reason,
              RejectReason::InvalidQty);
    NewOrder unknown = limit(Side::Buy, 10, 1, 0);
    unknown.instrument = InstrumentId{7};
    EXPECT_EQ(m.order(m.submit(unknown))->reject_reason, RejectReason::UnknownInstrument);

    const OrderId a = m.submit(limit(Side::Buy, 10, 1, 0));
    const OrderId b = m.submit(limit(Side::Sell, 900, 1, 0));
    m.cancel_all(CancelReason::EndOfRun);
    EXPECT_EQ(m.events().size(), 2U);
    EXPECT_EQ(m.order(a)->cancel_reason, CancelReason::EndOfRun);
    EXPECT_EQ(m.order(b)->cancel_reason, CancelReason::EndOfRun);
    EXPECT_EQ(m.open_order_count(), 0U);
}

// --- QuoteModel ---------------------------------------------------------------------------------

Quote quote(std::int64_t ts, std::int64_t bid, std::int64_t bid_qty, std::int64_t ask,
            std::int64_t ask_qty) {
    return Quote{kInst, Timestamp{ts}, Price{bid}, Qty{bid_qty}, Price{ask}, Qty{ask_qty}};
}

TEST(QuoteModelTest, QTE1_MarketTakesTheTouchCappedBySize) {
    QuoteModel m(test::make_registry());
    const OrderId buy = m.submit(market(Side::Buy, 8, 0));
    const OrderId sell = m.submit(market(Side::Sell, 2, 0));
    m.on_market_data(quote(5, 99, 10, 101, 5));
    const auto t = trades(m);
    ASSERT_EQ(t.size(), 2U);
    EXPECT_EQ(t[0].price, Price{101});
    EXPECT_EQ(t[0].qty, Qty{5});  // capped at the ask size
    EXPECT_EQ(t[1].price, Price{99});
    EXPECT_EQ(m.order(buy)->cancel_reason, CancelReason::MarketRemainder);  // 3 lots
    EXPECT_EQ(m.order(sell)->status, OrderStatus::Filled);
    EXPECT_EQ(m.mark(kInst), Price{100});  // floor((99 + 101) / 2)
}

TEST(QuoteModelTest, QTE2_LimitFillsWhenTheAskReachesIt) {
    QuoteModel m(test::make_registry());
    const OrderId at = m.submit(limit(Side::Buy, 101, 3, 0));     // ask == limit: fills
    const OrderId below = m.submit(limit(Side::Buy, 100, 3, 0));  // ask > limit: waits
    const OrderId sell = m.submit(limit(Side::Sell, 99, 1, 0));   // bid == limit: fills
    m.on_market_data(quote(5, 99, 10, 101, 10));
    EXPECT_EQ(m.order(at)->status, OrderStatus::Filled);
    EXPECT_EQ(trades(m)[0].price, Price{101});
    EXPECT_EQ(m.order(below)->status, OrderStatus::Accepted);
    EXPECT_EQ(m.order(sell)->status, OrderStatus::Filled);

    m.on_market_data(quote(9, 98, 10, 100, 2));  // ask drops to the limit, 2 lots shown
    EXPECT_EQ(m.order(below)->filled, Qty{2});
    EXPECT_EQ(m.order(below)->status, OrderStatus::PartiallyFilled);  // GTC keeps working
    m.on_market_data(quote(12, 98, 10, 99, 9));
    EXPECT_EQ(m.order(below)->status, OrderStatus::Filled);
    EXPECT_EQ(trades(m)[0].price, Price{99});  // at the ask, better than the limit
}

TEST(QuoteModelTest, QTE2_IocAndFokUseOnlyTheFirstQuote) {
    QuoteModel m(test::make_registry());
    const OrderId ioc = m.submit(limit(Side::Buy, 101, 5, 0, Tif::Ioc));
    const OrderId ioc_miss = m.submit(limit(Side::Buy, 100, 5, 0, Tif::Ioc));
    const OrderId fok = m.submit(limit(Side::Buy, 101, 5, 0, Tif::Fok, OwnerId{2}));
    m.on_market_data(quote(5, 99, 10, 101, 3));
    EXPECT_EQ(m.order(ioc)->filled, Qty{3});
    EXPECT_EQ(m.order(ioc)->cancel_reason, CancelReason::IocRemainder);
    EXPECT_EQ(m.order(ioc_miss)->filled, Qty{0});
    EXPECT_EQ(m.order(ioc_miss)->cancel_reason, CancelReason::IocRemainder);
    EXPECT_EQ(m.order(fok)->cancel_reason, CancelReason::FokUnfillable);  // 3 < 5
}

TEST(QuoteModelTest, QTE3_DisplayedSizeIsConsumedOncePerOwner) {
    QuoteModel m(test::make_registry());
    const OrderId a = m.submit(market(Side::Buy, 3, 0));
    const OrderId b = m.submit(limit(Side::Buy, 105, 4, 0));
    const OrderId c = m.submit(market(Side::Buy, 4, 0, OwnerId{2}));
    m.on_market_data(quote(5, 99, 10, 101, 5));
    EXPECT_EQ(m.order(a)->filled, Qty{3});
    EXPECT_EQ(m.order(b)->filled, Qty{2});  // 5 − 3
    EXPECT_EQ(m.order(c)->filled, Qty{4});  // another owner sees the whole size
}

TEST(QuoteModelTest, EmptySideDoesNotFillAndOrderArrivingAtQuoteTimeIsEligible) {
    QuoteModel m(test::make_registry());
    const OrderId buy = m.submit(market(Side::Buy, 1, 4));  // arrives at 5 == quote time
    m.on_market_data(quote(5, 99, 10, 0, 0));
    EXPECT_EQ(m.order(buy)->cancel_reason, CancelReason::MarketRemainder);
    EXPECT_FALSE(m.mark(kInst));  // no ask, no mid
    const OrderId buy2 = m.submit(market(Side::Buy, 1, 5));
    m.on_market_data(quote(6, 99, 10, 101, 1));
    EXPECT_EQ(m.order(buy2)->status, OrderStatus::Filled);
}

}  // namespace
}  // namespace finforge
