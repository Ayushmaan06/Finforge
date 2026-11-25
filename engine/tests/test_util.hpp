#pragma once

#include <span>
#include <variant>
#include <vector>

#include "finforge/instrument.hpp"
#include "finforge/order.hpp"

namespace finforge::test {

inline constexpr InstrumentId kInst{0};      // tick 1, lot 1, min_qty 1
inline constexpr InstrumentId kInstMin5{1};  // min_qty 5

inline InstrumentRegistry make_registry() {
    InstrumentRegistry reg;
    reg.add({.symbol = "SIM", .quote_currency = "USD", .tick_size = "1", .lot_size = "1"});
    reg.add({.symbol = "SIM5",
             .quote_currency = "USD",
             .tick_size = "1",
             .lot_size = "1",
             .min_qty = Qty{5}});
    return reg;
}

inline std::vector<Trade> trades_of(std::span<const BookEvent> events) {
    std::vector<Trade> out;
    for (const auto& e : events) {
        if (const auto* t = std::get_if<Trade>(&e)) out.push_back(*t);
    }
    return out;
}

}  // namespace finforge::test
