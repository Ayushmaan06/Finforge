// DET-2: fixed seed ⇒ fixed, known output sequence. Doubles are fine here: NUM-1 covers the
// engine, and tests use them only as an independent cross-check.

#include "finforge/rng.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace finforge;

TEST(Rng, DET2_SplitMix64ReferenceValues) {
    SplitMix64 s(0);  // published reference outputs for seed 0
    EXPECT_EQ(s.next(), 0xe220a8397b1dcdafULL);
    EXPECT_EQ(s.next(), 0x6e789e6aa1b965f4ULL);
    EXPECT_EQ(s.next(), 0x06c45d188009454fULL);
}

TEST(Rng, DET2_Pcg32ReferenceValues) {
    Pcg32 p(42, 54);  // pcg32-demo: pcg32_srandom(42, 54)
    const std::vector<std::uint32_t> expected = {0xa15c02b7, 0x7b47f409, 0xba1d3330,
                                                 0x83d2f293, 0xbfa4784b, 0xcbed606e};
    for (std::uint32_t e : expected) EXPECT_EQ(p.next(), e);
}

TEST(Rng, DET2_TransformsPinned) {
    Pcg32 a(42, 54);
    for (std::uint32_t e : {630u, 481u, 727u, 514u, 748u, 796u}) EXPECT_EQ(a.below(1000), e);
    Pcg32 b(42, 54);
    for (std::int64_t e : {461543, 730710, 318817, 663709, 289545, 227414}) {
        EXPECT_EQ(b.exponential(1'000'000), e);
    }
}

TEST(Rng, BelowIsInRangeAndCoversIt) {
    Pcg32 p(1, 2);
    std::vector<int> seen(7, 0);
    for (int i = 0; i < 7000; ++i) {
        const std::uint32_t x = p.below(7);
        ASSERT_LT(x, 7u);
        ++seen[x];
    }
    for (int c : seen) EXPECT_GT(c, 800);  // ~1000 each
    EXPECT_EQ(p.below(1), 0u);
}

TEST(Rng, RangeInclusive) {
    Pcg32 p(3, 4);
    bool lo = false, hi = false;
    for (int i = 0; i < 1000; ++i) {
        const std::int64_t x = p.range(-2, 2);
        ASSERT_GE(x, -2);
        ASSERT_LE(x, 2);
        lo |= x == -2;
        hi |= x == 2;
    }
    EXPECT_TRUE(lo && hi);
    EXPECT_EQ(p.range(5, 5), 5);
}

TEST(Rng, Log2Q32MatchesLibm) {
    EXPECT_EQ(log2_q32(1), 0u);
    EXPECT_EQ(log2_q32(2), std::uint64_t{1} << 32);
    EXPECT_EQ(log2_q32(std::uint64_t{1} << 32), std::uint64_t{32} << 32);
    for (std::uint64_t x : {3ULL, 5ULL, 1000ULL, 123456789ULL, 4294967295ULL, 4294967296ULL}) {
        const double got = static_cast<double>(log2_q32(x)) / 4294967296.0;
        EXPECT_NEAR(got, std::log2(static_cast<double>(x)), 1e-8) << x;
    }
}

TEST(Rng, ExponentialHasTheRightMean) {
    Pcg32 p(7, 11);
    const std::int64_t mean = 1'000'000;
    const int n = 200'000;
    double sum = 0;
    std::int64_t mx = 0;
    for (int i = 0; i < n; ++i) {
        const std::int64_t x = p.exponential(mean);
        ASSERT_GE(x, 0);
        sum += static_cast<double>(x);
        mx = std::max(mx, x);
    }
    EXPECT_NEAR(sum / n, static_cast<double>(mean), 0.01 * mean);  // σ/√n ≈ 0.22%
    EXPECT_LE(mx, 23 * mean);                                      // −ln(2^-32) ≈ 22.2
}

TEST(Rng, ExponentialAgainstLibm) {
    // Same draw, two ways: integer fixed point vs double. They agree to well under 1 ns per 1e6.
    Pcg32 a(9, 9), b(9, 9);
    for (int i = 0; i < 1000; ++i) {
        const std::int64_t got = a.exponential(1'000'000);
        const double u = (static_cast<double>(b.next()) + 1.0) / 4294967296.0;
        EXPECT_NEAR(static_cast<double>(got), -std::log(u) * 1e6, 1.0);
    }
}
