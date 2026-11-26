#include "finforge/sha256.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace finforge;

namespace {
std::string sha(std::string_view s) {
    Sha256 h;
    h.update(s);
    return h.hex();
}
}  // namespace

// FIPS 180-4 / NIST CAVP example vectors.
TEST(Sha256, NistVectors) {
    EXPECT_EQ(sha(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(sha("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                  "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
              "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

TEST(Sha256, MillionA) {
    Sha256 h;
    const std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) h.update(chunk);
    EXPECT_EQ(h.hex(), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, ChunkingDoesNotMatter) {
    std::string msg;
    for (int i = 0; i < 300; ++i) msg.push_back(static_cast<char>('a' + i % 26));
    const std::string whole = sha(msg);
    for (std::size_t step : {1u, 3u, 63u, 64u, 65u, 128u}) {
        Sha256 h;
        for (std::size_t i = 0; i < msg.size(); i += step) {
            h.update(std::string_view(msg).substr(i, step));
        }
        EXPECT_EQ(h.hex(), whole) << step;
    }
}

// The hardware path is what runs on most machines; the portable one must still be right (and is
// what the NIST vectors above exercise on CPUs without SHA extensions).
TEST(Sha256, HardwareAndPortableBlocksAgree) {
    if (!detail::sha256_hw_available()) GTEST_SKIP() << "no SHA extensions on this CPU";
    std::array<std::uint32_t, 8> hw{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<std::uint32_t, 8> portable = hw;
    std::array<std::uint8_t, 64> block{};
    std::uint64_t x = 0x9e3779b97f4a7c15;  // splitmix-style stream: varied, reproducible bytes
    for (int n = 0; n < 10'000; ++n) {
        for (auto& b : block) {
            x = x * 6364136223846793005u + 1442695040888963407u;
            b = static_cast<std::uint8_t>(x >> 56u);
        }
        detail::sha256_block_hw(hw, block.data());
        detail::sha256_block_portable(portable, block.data());
        ASSERT_EQ(hw, portable) << "block " << n;
    }
}

TEST(Sha256, DigestMidStreamDoesNotDisturbState) {
    Sha256 h;
    h.update("ab");
    EXPECT_EQ(h.hex(), sha("ab"));
    h.update("c");
    EXPECT_EQ(h.hex(), sha("abc"));
}
