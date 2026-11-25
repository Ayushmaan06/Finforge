// Unit tests for the matching engine: one or more tests per rule in contracts.md §4–§5.

#include "finforge/matching_engine.hpp"

#include <gtest/gtest.h>

#include <variant>
#include <vector>

#include "test_util.hpp"

using namespace finforge;
using namespace finforge::test;

class Book : public ::testing::Test {
  protected:
    MatchingEngine eng{make_registry()};

    OrderId limit(Side s, std::int64_t px, std::int64_t q, Tif tif = Tif::Gtc,
                  std::uint32_t owner = 1) {
        return eng.submit(NewOrder{OwnerId{owner}, kInst, s, OrderType::Limit, tif, Price{px},
                                   Qty{q}, Timestamp{0}});
    }
    OrderId market(Side s, std::int64_t q) {
        return eng.submit(NewOrder{OwnerId{9}, kInst, s, OrderType::Market, Tif::Gtc, Price{0},
                                   Qty{q}, Timestamp{0}});
    }
    const Order& ord(OrderId id) { return *eng.order(id); }
    std::vector<Trade> trades() { return trades_of(eng.events()); }
};

// --- Basic insertion, empty book ----------------------------------------------------------------

TEST_F(Book, EmptyBook) {
    const OrderBook& b = eng.book(kInst);
    EXPECT_TRUE(b.empty());
    EXPECT_FALSE(b.best_bid());
    EXPECT_FALSE(b.best_ask());
    EXPECT_FALSE(b.spread());
    EXPECT_TRUE(b.depth(Side::Buy, 5).empty());
    EXPECT_EQ(eng.order(OrderId{1}), nullptr);
    EXPECT_EQ(eng.order(OrderId{0}), nullptr);
}

TEST_F(Book, InsertRestsAndReportsTopOfBook) {
    const OrderId b = limit(Side::Buy, 100, 5);
    EXPECT_EQ(ord(b).status, OrderStatus::Accepted);
    ASSERT_EQ(eng.events().size(), 1u);
    EXPECT_EQ(std::get<OrderUpdate>(eng.events()[0]).status, OrderStatus::Accepted);
    limit(Side::Sell, 103, 2);
    const OrderBook& bk = eng.book(kInst);
    EXPECT_EQ(bk.best_bid(), Price{100});
    EXPECT_EQ(bk.best_ask(), Price{103});
    EXPECT_EQ(bk.spread(), Price{3});
    EXPECT_EQ(eng.open_order_count(), 2u);
}

TEST_F(Book, MultiplePriceLevelsDepth) {
    limit(Side::Buy, 100, 1);
    limit(Side::Buy, 102, 2);
    limit(Side::Buy, 101, 3);
    limit(Side::Buy, 102, 4);
    limit(Side::Sell, 105, 7);
    limit(Side::Sell, 104, 1);
    const OrderBook& bk = eng.book(kInst);
    EXPECT_EQ(bk.depth(Side::Buy, 10),
              (std::vector<LevelInfo>{
                  {Price{102}, Qty{6}, 2}, {Price{101}, Qty{3}, 1}, {Price{100}, Qty{1}, 1}}));
    EXPECT_EQ(bk.depth(Side::Buy, 2).size(), 2u);
    EXPECT_EQ(bk.depth(Side::Sell, 10),
              (std::vector<LevelInfo>{{Price{104}, Qty{1}, 1}, {Price{105}, Qty{7}, 1}}));
}

// --- NUM-7: dense ids
// -----------------------------------------------------------------------------

TEST_F(Book, NUM7_IdsDenseFromOne) {
    EXPECT_EQ(limit(Side::Buy, 100, 1), OrderId{1});
    EXPECT_EQ(limit(Side::Buy, 0, 1), OrderId{2});  // rejected orders get ids too
    EXPECT_EQ(limit(Side::Sell, 100, 1), OrderId{3});
    const auto t = trades();
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0].id, TradeId{1});
    EXPECT_EQ(ord(OrderId{3}).seq, Seq{3});
}

// --- Rejects (ORD-4, NUM-6, INS-3, ORD-6) -------------------------------------------------------

TEST_F(Book, NUM6_RejectsNonPositiveQty) {
    for (std::int64_t q : {0, -5}) {
        const OrderId id = limit(Side::Buy, 100, q);
        EXPECT_EQ(ord(id).status, OrderStatus::Rejected);
        EXPECT_EQ(ord(id).reject_reason, RejectReason::InvalidQty);
    }
    EXPECT_TRUE(eng.book(kInst).empty());
}

TEST_F(Book, INS3_RejectsBelowMinQty) {
    const OrderId id = eng.submit(NewOrder{OwnerId{1}, kInstMin5, Side::Buy, OrderType::Limit,
                                           Tif::Gtc, Price{10}, Qty{4}, Timestamp{0}});
    EXPECT_EQ(ord(id).reject_reason, RejectReason::InvalidQty);
    const OrderId ok = eng.submit(NewOrder{OwnerId{1}, kInstMin5, Side::Buy, OrderType::Limit,
                                           Tif::Gtc, Price{10}, Qty{5}, Timestamp{0}});
    EXPECT_EQ(ord(ok).status, OrderStatus::Accepted);
}

TEST_F(Book, NUM6_RejectsNonPositiveLimitPrice) {
    for (std::int64_t px : {0, -1}) {
        const OrderId id = limit(Side::Sell, px, 1);
        EXPECT_EQ(ord(id).reject_reason, RejectReason::InvalidPrice);
    }
}

TEST_F(Book, ORD6_RejectsMarketWithPrice) {
    const OrderId id = eng.submit(NewOrder{OwnerId{1}, kInst, Side::Buy, OrderType::Market,
                                           Tif::Gtc, Price{100}, Qty{1}, Timestamp{0}});
    EXPECT_EQ(ord(id).reject_reason, RejectReason::InvalidPrice);
}

TEST_F(Book, INS3_RejectsUnknownInstrument) {
    const OrderId id =
        eng.submit(NewOrder{OwnerId{1}, InstrumentId{42}, Side::Buy, OrderType::Limit, Tif::Gtc,
                            Price{1}, Qty{1}, Timestamp{0}});
    EXPECT_EQ(ord(id).status, OrderStatus::Rejected);
    EXPECT_EQ(ord(id).reject_reason, RejectReason::UnknownInstrument);
    ASSERT_EQ(eng.events().size(), 1u);
    const auto& u = std::get<OrderUpdate>(eng.events()[0]);
    EXPECT_EQ(u.status, OrderStatus::Rejected);
    EXPECT_EQ(u.reject_reason, RejectReason::UnknownInstrument);
}

// --- Cancellation (ORD-3, ORD-5) ----------------------------------------------------------------

TEST_F(Book, CancelRemovesOrderAndEmptyLevel) {
    const OrderId a = limit(Side::Buy, 100, 5);
    const OrderId b = limit(Side::Buy, 100, 3);
    EXPECT_TRUE(eng.cancel(a).ok());
    EXPECT_EQ(ord(a).status, OrderStatus::Cancelled);
    EXPECT_EQ(ord(a).cancel_reason, CancelReason::User);
    EXPECT_EQ(ord(a).cancelled, Qty{5});
    EXPECT_EQ(eng.book(kInst).depth(Side::Buy, 1)[0], (LevelInfo{Price{100}, Qty{3}, 1}));
    EXPECT_TRUE(eng.cancel(b).ok());
    EXPECT_TRUE(eng.book(kInst).empty());  // MAT-8
    EXPECT_EQ(eng.open_order_count(), 0u);
}

TEST_F(Book, CancelMiddleOfQueueKeepsFifo) {
    const OrderId a = limit(Side::Sell, 100, 1);
    const OrderId b = limit(Side::Sell, 100, 1);
    const OrderId c = limit(Side::Sell, 100, 1);
    eng.cancel(b);
    market(Side::Buy, 2);
    const auto t = trades();
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t[0].resting, a);
    EXPECT_EQ(t[1].resting, c);
}

TEST_F(Book, ORD3_ActionsOnUnknownOrTerminal) {
    EXPECT_EQ(eng.cancel(OrderId{7}).reason, RejectReason::UnknownOrder);
    EXPECT_EQ(eng.cancel(OrderId{0}).reason, RejectReason::UnknownOrder);
    const OrderId a = limit(Side::Buy, 100, 1);
    eng.cancel(a);
    EXPECT_EQ(eng.cancel(a).reason, RejectReason::AlreadyTerminal);
    EXPECT_TRUE(eng.events().empty());
    EXPECT_EQ(eng.amend(a, Qty{1}, Price{101}, Timestamp{0}).reason, RejectReason::AlreadyTerminal);
    const OrderId rej = limit(Side::Buy, 0, 1);
    EXPECT_EQ(eng.cancel(rej).reason, RejectReason::AlreadyTerminal);
    EXPECT_EQ(eng.amend(OrderId{99}, Qty{1}, Price{1}, Timestamp{0}).reason,
              RejectReason::UnknownOrder);
}

// --- Matching: priority, price, fills (MAT-1, MAT-2, MAT-7) -------------------------------------

TEST_F(Book, MAT1_PricePriorityThenTime) {
    const OrderId s1 = limit(Side::Sell, 101, 1);
    const OrderId s2 = limit(Side::Sell, 100, 1);
    const OrderId s3 = limit(Side::Sell, 100, 1);
    market(Side::Buy, 3);
    const auto t = trades();
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t[0].resting, s2);  // better price first
    EXPECT_EQ(t[1].resting, s3);  // then earlier seq
    EXPECT_EQ(t[2].resting, s1);
}

TEST_F(Book, MAT2_TradeAtRestingPrice) {
    limit(Side::Sell, 100, 1);
    const OrderId b = limit(Side::Buy, 105, 1);
    auto t = trades();
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0].price, Price{100});
    EXPECT_EQ(t[0].aggressor, b);
    EXPECT_EQ(t[0].aggressor_side, Side::Buy);

    limit(Side::Buy, 90, 1);
    limit(Side::Sell, 80, 1);
    t = trades();
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0].price, Price{90});
}

TEST_F(Book, FullFillRemovesRestingOrder) {
    const OrderId s = limit(Side::Sell, 100, 5);
    const OrderId b = limit(Side::Buy, 100, 5);
    EXPECT_EQ(ord(s).status, OrderStatus::Filled);
    EXPECT_EQ(ord(b).status, OrderStatus::Filled);
    EXPECT_TRUE(eng.book(kInst).empty());
    // Event order: Accepted, Trade, resting update, aggressor update.
    const auto ev = eng.events();
    ASSERT_EQ(ev.size(), 4u);
    EXPECT_EQ(std::get<OrderUpdate>(ev[0]).status, OrderStatus::Accepted);
    EXPECT_TRUE(std::holds_alternative<Trade>(ev[1]));
    EXPECT_EQ(std::get<OrderUpdate>(ev[2]).order, s);
    EXPECT_EQ(std::get<OrderUpdate>(ev[2]).status, OrderStatus::Filled);
    EXPECT_EQ(std::get<OrderUpdate>(ev[3]).order, b);
    EXPECT_EQ(std::get<OrderUpdate>(ev[3]).status, OrderStatus::Filled);
}

TEST_F(Book, PartialFillOfResting) {
    const OrderId s = limit(Side::Sell, 100, 5);
    const OrderId b = limit(Side::Buy, 100, 2);
    EXPECT_EQ(ord(s).status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(ord(s).remaining, Qty{3});
    EXPECT_EQ(ord(b).status, OrderStatus::Filled);
    EXPECT_EQ(eng.book(kInst).depth(Side::Sell, 1)[0], (LevelInfo{Price{100}, Qty{3}, 1}));
}

TEST_F(Book, PartialFillOfAggressorRestsRemainder) {
    limit(Side::Sell, 100, 2);
    limit(Side::Sell, 101, 2);
    const OrderId b = limit(Side::Buy, 100, 5);  // takes 2 @100, 101 is beyond its limit
    EXPECT_EQ(ord(b).status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(ord(b).remaining, Qty{3});
    const OrderBook& bk = eng.book(kInst);
    EXPECT_EQ(bk.best_bid(), Price{100});
    EXPECT_EQ(bk.best_ask(), Price{101});  // MAT-5: not crossed
}

TEST_F(Book, AggressorSweepsSeveralLevels) {
    limit(Side::Buy, 100, 1, Tif::Gtc, 2);
    limit(Side::Buy, 99, 2, Tif::Gtc, 3);
    limit(Side::Buy, 98, 3, Tif::Gtc, 4);
    const OrderId s = limit(Side::Sell, 99, 10, Tif::Gtc, 5);
    const auto t = trades();
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t[0].price, Price{100});
    EXPECT_EQ(t[0].resting_owner, OwnerId{2});  // MAT-6: owners recorded
    EXPECT_EQ(t[0].aggressor_owner, OwnerId{5});
    EXPECT_EQ(t[1].price, Price{99});
    EXPECT_EQ(t[1].qty, Qty{2});
    EXPECT_EQ(ord(s).remaining, Qty{7});
    EXPECT_EQ(eng.book(kInst).best_ask(), Price{99});
    EXPECT_EQ(eng.book(kInst).best_bid(), Price{98});
}

TEST_F(Book, MAT6_SelfTradeAllowed) {
    limit(Side::Sell, 100, 1, Tif::Gtc, 7);
    limit(Side::Buy, 100, 1, Tif::Gtc, 7);
    const auto t = trades();
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0].aggressor_owner, t[0].resting_owner);
}

// --- MARKET (MAT-3, ORD-6) ----------------------------------------------------------------------

TEST_F(Book, MAT3_MarketOnEmptyBookCancelsRemainder) {
    const OrderId m = market(Side::Buy, 3);
    EXPECT_EQ(ord(m).status, OrderStatus::Cancelled);
    EXPECT_EQ(ord(m).cancel_reason, CancelReason::MarketRemainder);
    EXPECT_EQ(ord(m).cancelled, Qty{3});
    EXPECT_TRUE(eng.book(kInst).empty());
}

TEST_F(Book, MAT3_MarketPartialThenCancel) {
    limit(Side::Sell, 100, 1);
    limit(Side::Sell, 500, 1);
    const OrderId m = market(Side::Buy, 5);
    EXPECT_EQ(trades().size(), 2u);  // no price limit
    EXPECT_EQ(ord(m).filled, Qty{2});
    EXPECT_EQ(ord(m).cancel_reason, CancelReason::MarketRemainder);
    EXPECT_EQ(ord(m).cancelled, Qty{3});
}

TEST_F(Book, ORD6_MarketIgnoresTif) {
    limit(Side::Buy, 100, 1);
    const OrderId m = eng.submit(NewOrder{OwnerId{1}, kInst, Side::Sell, OrderType::Market,
                                          Tif::Fok, Price{0}, Qty{4}, Timestamp{0}});
    EXPECT_EQ(ord(m).filled, Qty{1});  // FOK would have filled nothing
    EXPECT_EQ(ord(m).cancel_reason, CancelReason::MarketRemainder);
}

TEST_F(Book, MarketFullFill) {
    limit(Side::Buy, 100, 5);
    const OrderId m = market(Side::Sell, 5);
    EXPECT_EQ(ord(m).status, OrderStatus::Filled);
    EXPECT_TRUE(eng.book(kInst).empty());
}

// --- IOC / FOK (MAT-4) --------------------------------------------------------------------------

TEST_F(Book, MAT4_IocRemainderCancelled) {
    limit(Side::Sell, 100, 2);
    const OrderId b = limit(Side::Buy, 100, 5, Tif::Ioc);
    EXPECT_EQ(ord(b).filled, Qty{2});
    EXPECT_EQ(ord(b).status, OrderStatus::Cancelled);
    EXPECT_EQ(ord(b).cancel_reason, CancelReason::IocRemainder);
    EXPECT_FALSE(eng.book(kInst).best_bid());  // never rests
}

TEST_F(Book, MAT4_IocNoLiquidity) {
    const OrderId b = limit(Side::Buy, 100, 5, Tif::Ioc);
    EXPECT_EQ(ord(b).cancel_reason, CancelReason::IocRemainder);
    EXPECT_TRUE(eng.book(kInst).empty());
}

TEST_F(Book, MAT4_IocFullFill) {
    limit(Side::Sell, 100, 5);
    const OrderId b = limit(Side::Buy, 100, 5, Tif::Ioc);
    EXPECT_EQ(ord(b).status, OrderStatus::Filled);
}

TEST_F(Book, MAT4_FokUnfillableCancelsBeforeAnyFill) {
    const OrderId s1 = limit(Side::Sell, 100, 2);
    limit(Side::Sell, 102, 5);  // beyond the limit, must not count
    const OrderId b = limit(Side::Buy, 101, 3, Tif::Fok);
    EXPECT_EQ(ord(b).status, OrderStatus::Cancelled);
    EXPECT_EQ(ord(b).cancel_reason, CancelReason::FokUnfillable);
    EXPECT_EQ(ord(b).filled, Qty{0});
    EXPECT_TRUE(trades().empty());
    EXPECT_EQ(ord(s1).remaining, Qty{2});  // untouched
    ASSERT_EQ(eng.events().size(), 2u);    // Accepted, Cancelled
}

TEST_F(Book, MAT4_FokFillsAcrossLevels) {
    limit(Side::Sell, 100, 2);
    limit(Side::Sell, 101, 2);
    const OrderId b = limit(Side::Buy, 101, 4, Tif::Fok);
    EXPECT_EQ(ord(b).status, OrderStatus::Filled);
    EXPECT_EQ(trades().size(), 2u);
    EXPECT_TRUE(eng.book(kInst).empty());
}

TEST_F(Book, MAT4_FokExactlyAvailable) {
    limit(Side::Buy, 100, 3);
    const OrderId s = limit(Side::Sell, 100, 3, Tif::Fok);
    EXPECT_EQ(ord(s).status, OrderStatus::Filled);
}

// --- Amend (ORD-7) ------------------------------------------------------------------------------

TEST_F(Book, ORD7_ReduceKeepsPriority) {
    const OrderId a = limit(Side::Sell, 100, 5);
    const OrderId b = limit(Side::Sell, 100, 5);
    const ActionResult r = eng.amend(a, Qty{2}, Price{100}, Timestamp{1});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.new_order, OrderId{});
    ASSERT_EQ(eng.events().size(), 1u);
    EXPECT_EQ(std::get<OrderAmended>(eng.events()[0]), (OrderAmended{a, Qty{2}, Qty{2}}));
    EXPECT_EQ(ord(a).qty, Qty{2});
    EXPECT_EQ(eng.book(kInst).depth(Side::Sell, 1)[0], (LevelInfo{Price{100}, Qty{7}, 2}));
    market(Side::Buy, 1);
    EXPECT_EQ(trades()[0].resting, a);  // still first
    (void)b;
}

TEST_F(Book, ORD7_ReduceAfterPartialFill) {
    const OrderId a = limit(Side::Sell, 100, 10);
    market(Side::Buy, 4);
    EXPECT_EQ(eng.amend(a, Qty{4}, Price{100}, Timestamp{0}).reason, RejectReason::InvalidQty);
    ASSERT_TRUE(eng.amend(a, Qty{6}, Price{100}, Timestamp{0}).ok());
    EXPECT_EQ(ord(a).remaining, Qty{2});
    EXPECT_EQ(ord(a).filled + ord(a).remaining + ord(a).cancelled, ord(a).qty);  // ORD-8
}

TEST_F(Book, ORD7_PriceChangeIsCancelReplace) {
    const OrderId a = limit(Side::Sell, 101, 5);
    const OrderId b = limit(Side::Sell, 100, 5);
    const ActionResult r = eng.amend(a, Qty{5}, Price{100}, Timestamp{3});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(ord(a).status, OrderStatus::Cancelled);
    EXPECT_EQ(ord(a).cancel_reason, CancelReason::User);
    const Order& n = ord(r.new_order);
    EXPECT_GT(n.seq, ord(b).seq);  // lost priority
    EXPECT_EQ(n.price, Price{100});
    EXPECT_EQ(n.submit_ts, Timestamp{3});
    market(Side::Buy, 1);
    EXPECT_EQ(trades()[0].resting, b);
}

TEST_F(Book, ORD7_IncreaseIsCancelReplaceForUnfilledPart) {
    const OrderId a = limit(Side::Buy, 100, 5);
    market(Side::Sell, 2);
    const ActionResult r = eng.amend(a, Qty{8}, Price{100}, Timestamp{0});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(ord(a).cancelled, Qty{3});
    EXPECT_EQ(ord(r.new_order).qty, Qty{6});  // 8 total - 2 already filled
}

TEST_F(Book, ORD7_ReplaceCanMatch) {
    const OrderId a = limit(Side::Buy, 99, 5);
    limit(Side::Sell, 100, 5);
    const ActionResult r = eng.amend(a, Qty{5}, Price{100}, Timestamp{0});
    EXPECT_EQ(ord(r.new_order).status, OrderStatus::Filled);
    EXPECT_TRUE(eng.book(kInst).empty());
}

TEST_F(Book, ORD7_NoOpAndInvalidAmends) {
    const OrderId a = limit(Side::Buy, 100, 5);
    EXPECT_TRUE(eng.amend(a, Qty{5}, Price{100}, Timestamp{0}).ok());
    EXPECT_TRUE(eng.events().empty());
    EXPECT_EQ(eng.amend(a, Qty{0}, Price{100}, Timestamp{0}).reason, RejectReason::InvalidQty);
    EXPECT_EQ(eng.amend(a, Qty{3}, Price{0}, Timestamp{0}).reason, RejectReason::InvalidPrice);
    EXPECT_EQ(ord(a).status, OrderStatus::Accepted);
}

// --- ORD-2: one event per transition ------------------------------------------------------------

TEST_F(Book, ORD2_EventPerTransition) {
    limit(Side::Sell, 100, 1);
    limit(Side::Sell, 101, 1);
    limit(Side::Buy, 101, 3, Tif::Ioc);
    // Accepted, (Trade, resting Filled, aggr Partial) x2, Cancelled
    std::size_t updates = 0;
    for (const auto& e : eng.events()) updates += std::holds_alternative<OrderUpdate>(e);
    EXPECT_EQ(updates, 6u);
    EXPECT_EQ(eng.events().size(), 8u);
}

TEST_F(Book, Invariants) {
    for (int i = 0; i < 20; ++i) limit(i % 2 ? Side::Buy : Side::Sell, 100 + (i % 7) - 3, 1 + i);
    eng.check_invariants();
}
