#include "finforge/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace finforge {
namespace {

constexpr std::array<std::uint32_t, 64> kK = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr std::uint32_t rotr(std::uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }

}  // namespace

void Sha256::reset() {
    h_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    buf_len_ = 0;
    total_len_ = 0;
}

void Sha256::block(const std::uint8_t* p) {
    static const bool hw = detail::sha256_hw_available();
    if (hw) {
        detail::sha256_block_hw(h_, p);
    } else {
        detail::sha256_block_portable(h_, p);
    }
}

namespace detail {

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))

bool sha256_hw_available() {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid(1, &a, &b, &c, &d) == 0) return false;
    const bool ssse3 = (c & (1u << 9u)) != 0, sse41 = (c & (1u << 19u)) != 0;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d) == 0) return false;
    return ssse3 && sse41 && (b & (1u << 29u)) != 0;  // CPUID.7.0:EBX[29] = SHA
}

// Intel SHA extensions: sha256rnds2 does two rounds on state kept as ABEF/CDGH; msg1/msg2 do the
// message schedule four words at a time:
// W[i] = msg2(msg1(W[i-4], W[i-3]) + alignr(W[i-1], W[i-2], 4), W[i-1]).
__attribute__((target("sha,sse4.1,ssse3"))) void sha256_block_hw(std::array<std::uint32_t, 8>& h,
                                                                 const std::uint8_t* p) {
    const __m128i bswap = _mm_set_epi64x(0x0c0d0e0f08090a0bLL, 0x0405060700010203LL);
    __m128i tmp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&h[0]));
    __m128i state1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&h[4]));
    tmp = _mm_shuffle_epi32(tmp, 0xB1);                // CDAB
    state1 = _mm_shuffle_epi32(state1, 0x1B);          // EFGH
    __m128i state0 = _mm_alignr_epi8(tmp, state1, 8);  // ABEF
    state1 = _mm_blend_epi16(state1, tmp, 0xF0);       // CDGH
    const __m128i abef = state0, cdgh = state1;

    __m128i w[4];  // NOLINT: std::array<__m128i> drops the vector type's alignment attribute
    for (std::size_t i = 0; i < 16; ++i) {
        __m128i& cur = w[i % 4];
        if (i < 4) {
            cur = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16 * i)),
                                   bswap);
        } else {
            const __m128i& w1 = w[(i - 1) % 4];
            const __m128i s = _mm_add_epi32(_mm_sha256msg1_epu32(cur, w[(i - 3) % 4]),
                                            _mm_alignr_epi8(w1, w[(i - 2) % 4], 4));
            cur = _mm_sha256msg2_epu32(s, w1);
        }
        __m128i msg =
            _mm_add_epi32(cur, _mm_loadu_si128(reinterpret_cast<const __m128i*>(&kK[4 * i])));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        msg = _mm_shuffle_epi32(msg, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    }

    state0 = _mm_add_epi32(state0, abef);
    state1 = _mm_add_epi32(state1, cdgh);
    tmp = _mm_shuffle_epi32(state0, 0x1B);        // FEBA
    state1 = _mm_shuffle_epi32(state1, 0xB1);     // DCHG
    state0 = _mm_blend_epi16(tmp, state1, 0xF0);  // DCBA
    state1 = _mm_alignr_epi8(state1, tmp, 8);     // HGFE
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&h[0]), state0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&h[4]), state1);
}

#else

bool sha256_hw_available() { return false; }
void sha256_block_hw(std::array<std::uint32_t, 8>& h, const std::uint8_t* p) {
    sha256_block_portable(h, p);
}

#endif

void sha256_block_portable(std::array<std::uint32_t, 8>& h_, const std::uint8_t* p) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = (std::uint32_t{p[4 * i]} << 24u) | (std::uint32_t{p[4 * i + 1]} << 16u) |
               (std::uint32_t{p[4 * i + 2]} << 8u) | std::uint32_t{p[4 * i + 3]};
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3u);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10u);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    std::uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + ch + kK[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

}  // namespace detail

void Sha256::update(std::span<const std::uint8_t> data) {
    total_len_ += data.size();
    std::size_t i = 0;
    if (buf_len_ > 0) {
        const std::size_t take = std::min(data.size(), 64 - buf_len_);
        std::memcpy(buf_.data() + buf_len_, data.data(), take);
        buf_len_ += take;
        i = take;
        if (buf_len_ < 64) return;
        block(buf_.data());
        buf_len_ = 0;
    }
    for (; i + 64 <= data.size(); i += 64) block(data.data() + i);
    if (i < data.size()) {
        buf_len_ = data.size() - i;
        std::memcpy(buf_.data(), data.data() + i, buf_len_);
    }
}

Sha256::Digest Sha256::digest() const {
    Sha256 c = *this;  // finalise a copy; the running state continues
    const std::uint64_t bits = c.total_len_ * 8;
    std::array<std::uint8_t, 72> pad{};
    pad[0] = 0x80;
    const std::size_t pad_len = (c.buf_len_ < 56) ? 56 - c.buf_len_ : 120 - c.buf_len_;
    for (int i = 0; i < 8; ++i) pad[pad_len + i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    c.update({pad.data(), pad_len + 8});
    Digest d{};
    for (std::size_t i = 0; i < 8; ++i) {
        d[4 * i] = static_cast<std::uint8_t>(c.h_[i] >> 24u);
        d[4 * i + 1] = static_cast<std::uint8_t>(c.h_[i] >> 16u);
        d[4 * i + 2] = static_cast<std::uint8_t>(c.h_[i] >> 8u);
        d[4 * i + 3] = static_cast<std::uint8_t>(c.h_[i]);
    }
    return d;
}

std::string Sha256::to_hex(const Digest& d) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s(64, '0');
    for (std::size_t i = 0; i < d.size(); ++i) {
        s[2 * i] = kHex[d[i] >> 4u];
        s[2 * i + 1] = kHex[d[i] & 0xfu];
    }
    return s;
}

}  // namespace finforge
