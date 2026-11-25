#pragma once

// Deterministic randomness (contracts.md DET-2). The engine never uses std::*_distribution (their
// algorithms are implementation-defined, so results differ between libstdc++ and libc++) or libm
// (log/exp may differ in the last bit between platforms). Everything here is integer arithmetic
// with a fully specified result.

#include <cstdint>

namespace finforge {

// splitmix64 (Steele, Lea, Flood 2014). Used to derive independent seeds from one run seed.
class SplitMix64 {
  public:
    explicit constexpr SplitMix64(std::uint64_t seed) : state_(seed) {}

    constexpr std::uint64_t next() {
        std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

  private:
    std::uint64_t state_;
};

// PCG32, XSH-RR variant (O'Neill 2014), identical to pcg32_srandom_r / pcg32_random_r.
class Pcg32 {
  public:
    constexpr Pcg32(std::uint64_t init_state, std::uint64_t init_seq)
        : inc_((init_seq << 1u) | 1u) {
        next();
        state_ += init_state;
        next();
    }

    constexpr std::uint32_t next() {
        const std::uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + inc_;
        const auto xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
        const auto rot = static_cast<std::uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
    }

    // Uniform in [0, bound), bound >= 1, without modulo bias (Lemire 2019).
    std::uint32_t below(std::uint32_t bound);

    // Uniform in [lo, hi], inclusive; requires hi - lo < 2^32.
    std::int64_t range(std::int64_t lo, std::int64_t hi);

    // True with probability percent/100 (percent in 0..100).
    bool chance(std::uint32_t percent) { return below(100) < percent; }

    // Exponentially distributed integer with the given mean (e.g. a Poisson inter-arrival time in
    // ns): floor(mean × −ln(U)), U uniform on (0, 1] in steps of 2^-32. Integer fixed point only.
    std::int64_t exponential(std::int64_t mean);

  private:
    std::uint64_t state_ = 0;
    std::uint64_t inc_;
};

// log2(x) in Q32 fixed point (x >= 1): integer part in the high 32 bits, fraction in the low 32,
// truncated. Exposed for tests.
std::uint64_t log2_q32(std::uint64_t x);

}  // namespace finforge
