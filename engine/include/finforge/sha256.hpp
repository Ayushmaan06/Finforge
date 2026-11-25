#pragma once

// Streaming SHA-256 (FIPS 180-4). The journal hash is the run fingerprint (contracts.md DET-1).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace finforge {

class Sha256 {
  public:
    using Digest = std::array<std::uint8_t, 32>;

    Sha256() { reset(); }

    void reset();
    void update(std::span<const std::uint8_t> data);
    void update(std::string_view s) {
        update({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()});
    }
    // Digest of everything so far. Does not change the running state, so it can be called mid-run.
    [[nodiscard]] Digest digest() const;
    [[nodiscard]] std::string hex() const { return to_hex(digest()); }

    static std::string to_hex(const Digest& d);

  private:
    void block(const std::uint8_t* p);

    std::array<std::uint32_t, 8> h_{};
    std::array<std::uint8_t, 64> buf_{};
    std::size_t buf_len_ = 0;
    std::uint64_t total_len_ = 0;  // bytes
};

}  // namespace finforge
