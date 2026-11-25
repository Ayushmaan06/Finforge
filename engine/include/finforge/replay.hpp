#pragma once

// Historical replay (PLAN Phase 3 task 7). Market data arrives as column chunks (one feed per
// (kind, instrument)), typically numpy arrays handed over zero-copy from Python. Replay merges the
// feeds into one stream in a deterministic order and hands it to the simulator one event at a time,
// so only the current chunk of each feed is ever in memory.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "finforge/events.hpp"

namespace finforge {

enum class FeedKind : std::uint8_t { Bars = 0, Quotes = 1, Trades = 2 };

// Columns per kind, in this order (all int64):
//   Bars:   ts_open, ts_close, open, high, low, close, volume   (TIME-2: emitted at ts_close)
//   Quotes: ts, bid, bid_qty, ask, ask_qty
//   Trades: ts, price, qty   plus `aggressor` (int8: 0 buy, 1 sell, -1 unknown)
constexpr std::size_t column_count(FeedKind kind) {
    switch (kind) {
        case FeedKind::Bars:
            return 7;
        case FeedKind::Quotes:
            return 5;
        case FeedKind::Trades:
            return 3;
    }
    return 0;
}

struct ColumnChunk {
    std::size_t rows = 0;
    std::array<const std::int64_t*, 7> cols{};
    const std::int8_t* aggressor = nullptr;  // trades only
    // Owns the memory behind the pointers; released when the next chunk replaces this one.
    std::shared_ptr<const void> keepalive;
};

class ChunkProvider {
  public:
    virtual ~ChunkProvider() = default;
    // Fills `out` with the next chunk. Returns false at the end of the data. Empty chunks are
    // allowed and skipped.
    virtual bool next(ColumnChunk& out) = 0;
};

// A pull-based stream of market data in emit order (see Simulator::attach_source).
class MarketDataSource {
  public:
    virtual ~MarketDataSource() = default;
    // The next event, or false when the stream is exhausted.
    virtual bool next(MarketData& out) = 0;
};

// Merges feeds by (emit time, instrument id, attach order, row) (contracts.md §5.6). Within a feed,
// emit times must be non-decreasing; otherwise next() throws std::invalid_argument. Feeds must all
// be added before the first next().
class Replay final : public MarketDataSource {
  public:
    Replay();
    ~Replay() override;
    Replay(const Replay&) = delete;
    Replay& operator=(const Replay&) = delete;

    // `provider` must outlive the replay. Throws std::logic_error after the first next().
    void add_feed(FeedKind kind, InstrumentId instrument, ChunkProvider& provider);

    bool next(MarketData& out) override;

    [[nodiscard]] std::size_t feed_count() const { return feeds_.size(); }
    // Events handed out so far, per kind.
    [[nodiscard]] std::uint64_t emitted(FeedKind kind) const {
        return emitted_[static_cast<std::size_t>(kind)];
    }

  private:
    class Feed;

    bool before(std::size_t a, std::size_t b) const;  // heap order: a comes first

    std::vector<std::unique_ptr<Feed>> feeds_;
    std::vector<std::size_t> heap_;  // indices of non-exhausted feeds, min-heap by before()
    std::array<std::uint64_t, 3> emitted_{};
    bool started_ = false;
};

}  // namespace finforge
