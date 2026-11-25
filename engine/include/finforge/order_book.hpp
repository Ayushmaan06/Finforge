#pragma once

// One instrument's limit order book (contracts.md §5). Levels live in std::map, best price first;
// each level is an intrusive FIFO of order ids whose links live in the Order records, which are
// owned by MatchingEngine. The book itself never allocates per order.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "finforge/order.hpp"
#include "finforge/types.hpp"

namespace finforge {

struct Level {
    Qty total;  // sum of `remaining` of the orders in the level; > 0 (MAT-8)
    std::uint32_t count = 0;
    OrderId head;  // oldest (lowest seq), matched first (MAT-1)
    OrderId tail;
};

struct LevelInfo {
    Price price;
    Qty qty;
    std::uint32_t orders = 0;

    friend bool operator==(const LevelInfo&, const LevelInfo&) = default;
};

class OrderBook {
  public:
    explicit OrderBook(InstrumentId instrument) : instrument_(instrument) {}

    [[nodiscard]] InstrumentId instrument() const { return instrument_; }
    [[nodiscard]] std::optional<Price> best_bid() const;
    [[nodiscard]] std::optional<Price> best_ask() const;
    // best_ask - best_bid, when both sides exist.
    [[nodiscard]] std::optional<Price> spread() const;
    // Up to n levels of one side, best first.
    [[nodiscard]] std::vector<LevelInfo> depth(Side side, std::size_t n) const;
    [[nodiscard]] bool empty() const { return bids_.empty() && asks_.empty(); }

  private:
    friend class MatchingEngine;

    // Bids: highest first. Asks: lowest first. In both maps, begin() is the best level, and a level
    // at `price` is acceptable to an aggressor with limit `limit` iff !comp(limit, price).
    using Bids = std::map<Price, Level, std::greater<>>;
    using Asks = std::map<Price, Level, std::less<>>;

    InstrumentId instrument_;
    Bids bids_;
    Asks asks_;
};

}  // namespace finforge
