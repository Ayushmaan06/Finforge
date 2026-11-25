#pragma once

// Numeric core (contracts.md §1). Every quantity is an integer in a fixed unit; floating point
// never appears in the engine (NUM-1). Strong types make mixing units a compile error:
// `Price{1} + Qty{1}` does not compile.

#include <compare>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace finforge {

// NUM-4: a result that does not fit its type is a hard error, never a wrap.
class NumericOverflow : public std::overflow_error {
  public:
    explicit NumericOverflow(const std::string& what) : std::overflow_error(what) {}
};

__extension__ using int128 = __int128;

inline std::int64_t checked_add(std::int64_t a, std::int64_t b) {
    std::int64_t r;
    if (__builtin_add_overflow(a, b, &r)) throw NumericOverflow("NUMERIC_OVERFLOW: add");
    return r;
}

inline std::int64_t checked_sub(std::int64_t a, std::int64_t b) {
    std::int64_t r;
    if (__builtin_sub_overflow(a, b, &r)) throw NumericOverflow("NUMERIC_OVERFLOW: sub");
    return r;
}

inline std::int64_t checked_mul(std::int64_t a, std::int64_t b) {
    std::int64_t r;
    if (__builtin_mul_overflow(a, b, &r)) throw NumericOverflow("NUMERIC_OVERFLOW: mul");
    return r;
}

// A signed 64-bit amount in a fixed unit. Same-type addition and subtraction are checked; there is
// no multiplication between amounts (that needs units, see notional()).
template <class Tag>
class Amount {
  public:
    using rep = std::int64_t;

    constexpr Amount() = default;
    constexpr explicit Amount(rep v) : v_(v) {}

    [[nodiscard]] constexpr rep value() const { return v_; }

    friend constexpr bool operator==(const Amount&, const Amount&) = default;
    friend constexpr auto operator<=>(const Amount&, const Amount&) = default;

    friend Amount operator+(Amount a, Amount b) { return Amount{checked_add(a.v_, b.v_)}; }
    friend Amount operator-(Amount a, Amount b) { return Amount{checked_sub(a.v_, b.v_)}; }
    Amount& operator+=(Amount o) { return *this = *this + o; }
    Amount& operator-=(Amount o) { return *this = *this - o; }

  private:
    rep v_ = 0;
};

// An identifier or counter. Only comparison; no arithmetic.
template <class Tag, class Rep>
class Id {
  public:
    using rep = Rep;

    constexpr Id() = default;
    constexpr explicit Id(rep v) : v_(v) {}

    [[nodiscard]] constexpr rep value() const { return v_; }

    friend constexpr bool operator==(const Id&, const Id&) = default;
    friend constexpr auto operator<=>(const Id&, const Id&) = default;

  private:
    rep v_ = 0;
};

using Price = Amount<struct PriceTag>;  // ticks
using Qty = Amount<struct QtyTag>;      // lots
using Money = Amount<struct MoneyTag>;  // 1e-8 of the quote currency
using Rate = Amount<struct RateTag>;    // 1e-6; 10 bps = Rate{1000}

using OrderId = Id<struct OrderIdTag, std::uint64_t>;  // 0 = none; assigned from 1 (NUM-7)
using TradeId = Id<struct TradeIdTag, std::uint64_t>;
using Seq = Id<struct SeqTag, std::uint64_t>;
using InstrumentId = Id<struct InstrumentIdTag, std::uint32_t>;  // dense from 0 (INS-1)
using OwnerId = Id<struct OwnerIdTag, std::uint32_t>;
using Timestamp = Id<struct TimestampTag, std::int64_t>;  // ns since Unix epoch, UTC (TIME-1)

// NUM-3: price_ticks × qty_lots × money_per_tick_lot × contract_multiplier, computed in 128 bits
// and range-checked into Money.
inline Money notional(Price price, Qty qty, std::int64_t money_per_tick_lot,
                      std::int64_t contract_multiplier) {
    int128 r = static_cast<int128>(price.value()) * qty.value();  // |r| < 2^126: cannot overflow
    if (__builtin_mul_overflow(r, static_cast<int128>(money_per_tick_lot), &r) ||
        __builtin_mul_overflow(r, static_cast<int128>(contract_multiplier), &r) ||
        r > std::numeric_limits<std::int64_t>::max() ||
        r < std::numeric_limits<std::int64_t>::min()) {
        throw NumericOverflow("NUMERIC_OVERFLOW: notional");
    }
    return Money{static_cast<std::int64_t>(r)};
}

}  // namespace finforge
