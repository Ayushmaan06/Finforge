#include "finforge/rng.hpp"

#include <cstdint>
#include <limits>

#include "finforge/assert.hpp"

namespace finforge {
namespace {
__extension__ using u128 = unsigned __int128;

// ln(2) in Q64, rounded: 0.B17217F7D1CF79ABC9E3... (hex)
constexpr std::uint64_t kLn2Q64 = 0xB17217F7D1CF79ACULL;
}  // namespace

std::uint32_t Pcg32::below(std::uint32_t bound) {
    FINFORGE_CHECK(bound >= 1, "bound must be >= 1");
    std::uint64_t m = static_cast<std::uint64_t>(next()) * bound;
    auto low = static_cast<std::uint32_t>(m);
    if (low < bound) {
        const std::uint32_t threshold = (0u - bound) % bound;  // 2^32 mod bound
        while (low < threshold) {
            m = static_cast<std::uint64_t>(next()) * bound;
            low = static_cast<std::uint32_t>(m);
        }
    }
    return static_cast<std::uint32_t>(m >> 32u);
}

std::int64_t Pcg32::range(std::int64_t lo, std::int64_t hi) {
    FINFORGE_CHECK(lo <= hi, "empty range");
    const auto span = static_cast<std::uint64_t>(hi) - static_cast<std::uint64_t>(lo);
    FINFORGE_CHECK(span < (std::uint64_t{1} << 32u), "range too wide");
    if (span == std::numeric_limits<std::uint32_t>::max()) return lo + next();
    return lo + below(static_cast<std::uint32_t>(span + 1));
}

std::uint64_t log2_q32(std::uint64_t x) {
    FINFORGE_CHECK(x >= 1, "log2 of zero");
    const int ip = 63 - __builtin_clzll(x);
    // Mantissa x / 2^ip in [1, 2), as Q62 in 128 bits.
    u128 m = static_cast<u128>(x) << (62 - ip);
    std::uint64_t frac = 0;
    // Each squaring doubles log2(m); if m reaches [2, 4) the next fractional bit is 1.
    for (int i = 0; i < 32; ++i) {
        m = (m * m) >> 62u;
        frac <<= 1u;
        if (m >= (static_cast<u128>(1) << 63u)) {
            m >>= 1u;
            frac |= 1u;
        }
    }
    return (static_cast<std::uint64_t>(ip) << 32u) | frac;
}

std::int64_t Pcg32::exponential(std::int64_t mean) {
    FINFORGE_CHECK(mean >= 0, "negative mean");
    const std::uint64_t u = std::uint64_t{next()} + 1;  // (0, 2^32] → U = u / 2^32 in (0, 1]
    // −ln(U) = ln2 × (32 − log2 u), all in Q32.
    const std::uint64_t neg_log2 = (std::uint64_t{32} << 32u) - log2_q32(u);  // < 2^37
    const auto neg_ln = static_cast<std::uint64_t>((static_cast<u128>(neg_log2) * kLn2Q64) >> 64u);
    const u128 r = (static_cast<u128>(neg_ln) * static_cast<std::uint64_t>(mean)) >> 32u;
    FINFORGE_CHECK(r <= static_cast<u128>(std::numeric_limits<std::int64_t>::max()),
                   "exponential overflow");
    return static_cast<std::int64_t>(r);
}

}  // namespace finforge
