#include "finforge/order_book.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace finforge {

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::optional<Price> OrderBook::spread() const {
    if (bids_.empty() || asks_.empty()) return std::nullopt;
    return asks_.begin()->first - bids_.begin()->first;
}

std::vector<LevelInfo> OrderBook::depth(Side side, std::size_t n) const {
    std::vector<LevelInfo> out;
    auto collect = [&](const auto& levels) {
        for (auto it = levels.begin(); it != levels.end() && out.size() < n; ++it) {
            out.push_back(LevelInfo{it->first, it->second.total, it->second.count});
        }
    };
    if (side == Side::Buy) {
        collect(bids_);
    } else {
        collect(asks_);
    }
    return out;
}

}  // namespace finforge
