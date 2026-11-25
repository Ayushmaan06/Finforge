#pragma once

// Append-only event journal with a running SHA-256: the run fingerprint (contracts.md DET-1).
// Byte format (OP-2, decided in Phase 2; documented in contracts.md §10.1):
//
//   header : "FFJ1" magic (4 bytes) | u16 format version | u16 reserved (0)
//   record : u8 type | u8 reserved (0) | u16 payload length | payload
//
// Every integer is little-endian and fixed width; there is no padding and no floating point.
// Input records start with the event key: i64 ts | u8 class | u32 instrument | u64 seq.

#include <cstdint>
#include <ostream>
#include <span>
#include <string>
#include <vector>

#include "finforge/events.hpp"
#include "finforge/order.hpp"
#include "finforge/order_book.hpp"
#include "finforge/sha256.hpp"

namespace finforge {

enum class RecordType : std::uint8_t {
    // Inputs (processed queue events)
    Submit = 1,
    Cancel = 2,
    Amend = 3,
    Bar = 4,
    Quote = 5,
    TradePrint = 6,
    Timer = 7,
    Snapshot = 8,
    // Outputs (what the execution model emitted)
    OrderUpdate = 16,
    OrderAmended = 17,
    Trade = 18,
    ActionRefused = 19,
};

class Journal {
  public:
    static constexpr std::uint16_t kFormatVersion = 1;

    // `sink` (optional) receives the exact bytes that are hashed.
    explicit Journal(std::ostream* sink = nullptr);

    void record_input(const EventKey& key, const Event& event);
    void record_output(const BookEvent& event);
    void record_refused(const EventKey& key, OwnerId owner, OrderId order, RejectReason reason);
    // Snapshot input record: the key, then per book: u32 instrument | u8 has_bid | i64 bid |
    // i64 bid_qty | u8 has_ask | i64 ask | i64 ask_qty.
    void record_snapshot(const EventKey& key, std::span<const OrderBook* const> books);

    [[nodiscard]] std::string hash_hex() const { return hash_.hex(); }
    [[nodiscard]] std::uint64_t records() const { return records_; }
    [[nodiscard]] std::uint64_t bytes() const { return bytes_; }

  private:
    void begin(RecordType type);
    void finish();
    void put_key(const EventKey& key);
    void put_u8(std::uint8_t v) { buf_.push_back(v); }
    void put_u16(std::uint16_t v) { put_le(v, 2); }
    void put_u32(std::uint32_t v) { put_le(v, 4); }
    void put_u64(std::uint64_t v) { put_le(v, 8); }
    void put_i64(std::int64_t v) { put_le(static_cast<std::uint64_t>(v), 8); }
    void put_le(std::uint64_t v, int n) {
        for (int i = 0; i < n; ++i) buf_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void write(std::span<const std::uint8_t> bytes);

    Sha256 hash_;
    std::ostream* sink_;
    std::vector<std::uint8_t> buf_;
    std::uint64_t records_ = 0;
    std::uint64_t bytes_ = 0;
};

}  // namespace finforge
