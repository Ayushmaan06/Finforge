// Differential test (PLAN Phase 1 task 8, contracts.md §5 Tests): a seeded stream of random
// operations runs against MatchingEngine and ReferenceBook. Returned ids/results, every event,
// and the top of book must agree after every operation; full depth and all order records are
// compared periodically and at the end. MatchingEngine::check_invariants() runs after every
// operation in Debug builds (FINFORGE_EXPENSIVE_CHECKS), which covers ORD-8 and MAT-5/7/8.
//
// FINFORGE_DIFF_OPS overrides the number of operations per seed (default 1'000'000).

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>

#include "finforge/matching_engine.hpp"
#include "reference_book.hpp"
#include "test_util.hpp"

using namespace finforge;
using namespace finforge::test;

namespace {

std::uint64_t op_count() {
    if (const char* s = std::getenv("FINFORGE_DIFF_OPS")) return std::stoull(s);
    return 1'000'000;
}

// mt19937_64's output sequence is specified by the standard; std:: distributions are not, so
// draws are mapped to ranges by hand (modulo bias is irrelevant here).
class Gen {
  public:
    explicit Gen(std::uint64_t seed) : rng_(seed) {}
    std::int64_t range(std::int64_t lo, std::int64_t hi) {  // inclusive
        return lo + static_cast<std::int64_t>(rng_() % static_cast<std::uint64_t>(hi - lo + 1));
    }
    bool chance(int percent) { return range(1, 100) <= percent; }

  private:
    std::mt19937_64 rng_;
};

constexpr std::int64_t kMid = 1000;
constexpr std::size_t kMaxResting = 100;
constexpr std::uint64_t kWindow = 250;  // cancel/amend target the most recent ids

void expect_same_order(const Order& a, const Order& b) {
    ASSERT_EQ(a.status, b.status) << "order " << a.id.value();
    ASSERT_EQ(a.filled, b.filled) << "order " << a.id.value();
    ASSERT_EQ(a.remaining, b.remaining) << "order " << a.id.value();
    ASSERT_EQ(a.cancelled, b.cancelled) << "order " << a.id.value();
    ASSERT_EQ(a.qty, b.qty) << "order " << a.id.value();
    ASSERT_EQ(a.reject_reason, b.reject_reason) << "order " << a.id.value();
    ASSERT_EQ(a.cancel_reason, b.cancel_reason) << "order " << a.id.value();
}

class Differential : public ::testing::TestWithParam<std::uint64_t> {};

TEST_P(Differential, MatchesReferenceBook) {
    const std::uint64_t seed = GetParam();
    const std::uint64_t n_ops = op_count();
    Gen g(seed);
    MatchingEngine eng(make_registry());
    ReferenceBook ref(make_registry());
    std::uint64_t trades = 0;

    for (std::uint64_t i = 0; i < n_ops; ++i) {
        // Streamed into every assertion; gtest only evaluates it on failure.
        auto at_op = [&] { return "seed " + std::to_string(seed) + " op " + std::to_string(i); };
        const Timestamp ts{static_cast<std::int64_t>(i)};
        const std::uint64_t n_orders = eng.order_count();
        // Bias towards cancels when the book gets large, so the O(n) reference stays fast.
        const int cancel_pct = eng.open_order_count() > kMaxResting ? 70 : 25;
        const int r = static_cast<int>(g.range(1, 100));

        if (r <= cancel_pct) {
            // Recent ids are mostly still open; include 0 and one-past-the-end for UNKNOWN_ORDER.
            const auto lo = static_cast<std::int64_t>(n_orders > kWindow ? n_orders - kWindow : 0);
            const OrderId id{static_cast<std::uint64_t>(g.range(lo, n_orders + 1))};
            const ActionResult a = eng.cancel(id);
            const ActionResult b = ref.cancel(id);
            ASSERT_EQ(a.reason, b.reason) << at_op();
        } else if (r <= cancel_pct + 10) {
            const auto lo = static_cast<std::int64_t>(n_orders > kWindow ? n_orders - kWindow : 0);
            const OrderId id{static_cast<std::uint64_t>(g.range(lo, n_orders + 1))};
            Qty q{g.range(0, 25)};
            Price p{kMid + g.range(-12, 12)};
            if (const Order* o = eng.order(id); o && g.chance(50)) {
                p = o->price;  // same price: reduce in place, no-op, or increase
                q = Qty{g.range(0, std::max<std::int64_t>(o->qty.value(), 0) + 3)};
            }
            if (g.chance(2)) p = Price{0};
            const ActionResult a = eng.amend(id, q, p, ts);
            const ActionResult b = ref.amend(id, q, p, ts);
            ASSERT_EQ(a.reason, b.reason) << at_op();
            ASSERT_EQ(a.new_order, b.new_order) << at_op();
        } else {
            NewOrder o;
            o.owner = OwnerId{static_cast<std::uint32_t>(g.range(1, 5))};
            o.instrument = kInst;
            o.side = g.chance(50) ? Side::Buy : Side::Sell;
            o.ts = ts;
            if (g.chance(8)) {
                o.type = OrderType::Market;
                o.qty = Qty{g.range(1, 30)};
            } else {
                o.type = OrderType::Limit;
                // Buys centred a little below mid, sells a little above: some cross, most rest.
                const std::int64_t off = g.range(-10, 10);
                o.price = Price{o.side == Side::Buy ? kMid - 2 + off : kMid + 2 + off};
                o.qty = Qty{g.range(1, 20)};
            }
            const int t = static_cast<int>(g.range(1, 100));
            o.tif = t <= 70 ? Tif::Gtc : t <= 85 ? Tif::Ioc : Tif::Fok;
            if (g.chance(3)) {  // invalid inputs
                switch (g.range(0, 4)) {
                    case 0:
                        o.qty = Qty{0};
                        break;
                    case 1:
                        o.qty = Qty{-g.range(1, 5)};
                        break;
                    case 2:
                        o.price = Price{o.type == OrderType::Market ? 1000 : 0};
                        break;
                    case 3:
                        o.instrument = InstrumentId{7};
                        break;
                    case 4:
                        o.instrument = kInstMin5;
                        o.qty = Qty{g.range(1, 9)};
                        break;
                }
            }
            ASSERT_EQ(eng.submit(o), ref.submit(o)) << at_op();
        }

        // Every event, in order.
        const auto ev = eng.events();
        ASSERT_EQ(ev.size(), ref.events().size()) << at_op();
        for (std::size_t k = 0; k < ev.size(); ++k) {
            ASSERT_TRUE(ev[k] == ref.events()[k]) << at_op() << " event " << k;
            trades += std::holds_alternative<Trade>(ev[k]);
        }
        for (InstrumentId inst : {kInst, kInstMin5}) {
            const OrderBook& bk = eng.book(inst);
            // Top of book every op for the main instrument; the rarely used one less often.
            if (inst == kInst || i % 64 == 0) {
                ASSERT_EQ(bk.best_bid(), ref.best(inst, Side::Buy)) << at_op();
                ASSERT_EQ(bk.best_ask(), ref.best(inst, Side::Sell)) << at_op();
            }
            if (i % 4096 == 0 || i + 1 == n_ops) {
                ASSERT_EQ(bk.depth(Side::Buy, 1000), ref.depth(inst, Side::Buy, 1000)) << at_op();
                ASSERT_EQ(bk.depth(Side::Sell, 1000), ref.depth(inst, Side::Sell, 1000)) << at_op();
            }
        }
        if (i % 4096 == 0 || i + 1 == n_ops) eng.check_invariants();
    }

    // All order records, including terminal ones; ids dense (NUM-7).
    ASSERT_EQ(eng.order(OrderId{eng.order_count() + 1}), nullptr);
    for (std::uint64_t id = 1; id <= eng.order_count(); ++id) {
        ASSERT_NE(ref.order(OrderId{id}), nullptr);
        expect_same_order(*eng.order(OrderId{id}), *ref.order(OrderId{id}));
    }
    // Sanity: the stream exercised matching, not just inserts.
    EXPECT_GT(trades, n_ops / 20);
    RecordProperty("orders", std::to_string(eng.order_count()));
    RecordProperty("trades", std::to_string(trades));
}

INSTANTIATE_TEST_SUITE_P(Seeds, Differential, ::testing::Values(1u, 42u, 20261125u));

}  // namespace
