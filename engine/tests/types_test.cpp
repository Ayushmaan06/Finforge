#include "finforge/types.hpp"

#include <gtest/gtest.h>

#include <concepts>
#include <cstdint>
#include <limits>

using namespace finforge;

namespace {
constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();

template <class A, class B>
concept Addable = requires(A a, B b) { a + b; };
}  // namespace

// Strong types: units do not mix.
static_assert(Addable<Price, Price>);
static_assert(Addable<Qty, Qty>);
static_assert(!Addable<Price, Qty>);
static_assert(!Addable<Money, Price>);
static_assert(!Addable<OrderId, OrderId>);                 // ids have no arithmetic
static_assert(!std::convertible_to<std::int64_t, Price>);  // explicit construction only
static_assert(!std::convertible_to<Price, std::int64_t>);

TEST(Types, AmountArithmeticAndOrdering) {
    EXPECT_EQ(Price{5} + Price{3}, Price{8});
    EXPECT_EQ(Qty{5} - Qty{7}, Qty{-2});
    EXPECT_LT(Price{1}, Price{2});
    Money m{10};
    m += Money{5};
    m -= Money{1};
    EXPECT_EQ(m, Money{14});
    EXPECT_EQ(Price{}.value(), 0);
}

// NUM-4: overflow never wraps.
TEST(Types, NUM4_AddSubOverflowThrows) {
    EXPECT_THROW(Money{kMax} + Money{1}, NumericOverflow);
    EXPECT_THROW(Money{kMin} - Money{1}, NumericOverflow);
    EXPECT_THROW(Qty{kMin} + Qty{-1}, NumericOverflow);
    EXPECT_EQ(Money{kMax} + Money{0}, Money{kMax});
    EXPECT_THROW(checked_mul(kMax, 2), NumericOverflow);
    EXPECT_EQ(checked_mul(kMax, 1), kMax);
}

// NUM-3: notional is exact in 128 bits, then range-checked.
TEST(Types, NUM3_NotionalExact) {
    // BTCUSDT spot: tick 0.01, lot 0.00001 => money_per_tick_lot 10.
    // 65 000.00 USDT × 0.5 BTC = 32 500 USDT = 3.25e12 Money units.
    EXPECT_EQ(notional(Price{6'500'000}, Qty{50'000}, 10, 1), Money{3'250'000'000'000});
    EXPECT_EQ(notional(Price{0}, Qty{kMax}, 10'000, 1), Money{0});
    EXPECT_EQ(notional(Price{-3}, Qty{4}, 10, 1), Money{-120});
}

TEST(Types, NUM3_NotionalOverflowMidComputation) {
    // price × qty overflows int64 but the 128-bit path is exact; the result does not fit Money.
    EXPECT_THROW(notional(Price{kMax}, Qty{2}, 1, 1), NumericOverflow);
    // price × qty fits int64; × money_per_tick_lot does not.
    EXPECT_THROW(notional(Price{1'000'000'000}, Qty{1'000'000'000}, 10, 1), NumericOverflow);
    // Exactly at the boundary.
    EXPECT_EQ(notional(Price{kMax}, Qty{1}, 1, 1), Money{kMax});
    EXPECT_EQ(notional(Price{kMin}, Qty{1}, 1, 1), Money{kMin});
    EXPECT_THROW(notional(Price{kMin}, Qty{-1}, 1, 1), NumericOverflow);
    // Intermediate exceeds 2^127 in the third factor: must not wrap silently.
    EXPECT_THROW(notional(Price{kMax}, Qty{kMax}, kMax, 1), NumericOverflow);
    EXPECT_THROW(notional(Price{kMax}, Qty{kMax}, 1, kMax), NumericOverflow);
}
