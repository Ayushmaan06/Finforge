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

TEST(Sha256, DigestMidStreamDoesNotDisturbState) {
    Sha256 h;
    h.update("ab");
    EXPECT_EQ(h.hex(), sha("ab"));
    h.update("c");
    EXPECT_EQ(h.hex(), sha("abc"));
}
