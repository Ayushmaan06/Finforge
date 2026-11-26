#include "finforge/execution_model.hpp"

#include <variant>

namespace finforge {

std::optional<Price> BookModel::mark(InstrumentId id) const {
    if (id.value() >= engine_.registry().size()) return std::nullopt;
    const OrderBook& b = engine_.book(id);
    const auto bid = b.depth(Side::Buy, 1);
    const auto ask = b.depth(Side::Sell, 1);
    if (bid.empty() || ask.empty()) return std::nullopt;
    // ACC-6: floor of the mid, in ticks. Both are positive, so the sum's halving floors.
    return Price{(bid[0].price.value() + ask[0].price.value()) / 2};
}

void BookModel::on_hooks_changed() {
    if (hooks_ == nullptr) {
        engine_.set_gate({});
        return;
    }
    engine_.set_gate([this](const NewOrder& o) {
        // ACC-9 on an amend replacement: the old order's cancel is already in events() but not yet
        // reported, so its reservation and open quantity would still count. Report status updates
        // now; on_order_update works from the order's state, so report() repeating them is a no-op.
        for (const BookEvent& e : engine_.events()) {
            if (const auto* u = std::get_if<OrderUpdate>(&e)) {
                hooks_->on_order_update(*engine_.order(u->order));
            }
        }
        // LIMIT: fills happen at resting prices no worse than the limit, so the limit is a true
        // worst case. MARKET: the mark (no slippage in Mode B); 0 before the book has two sides.
        const Price ref =
            o.type == OrderType::Limit ? o.price : mark(o.instrument).value_or(Price{0});
        return hooks_->pre_trade(o, ref);
    });
}

void BookModel::report() {
    if (hooks_ == nullptr) return;
    for (const BookEvent& e : engine_.events()) {
        if (const auto* t = std::get_if<Trade>(&e)) {
            // BKM-1: the aggressor pays taker, the resting side maker.
            hooks_->on_fill(*engine_.order(t->resting), *t, Liquidity::Maker);
            hooks_->on_fill(*engine_.order(t->aggressor), *t, Liquidity::Taker);
        } else if (const auto* u = std::get_if<OrderUpdate>(&e)) {
            hooks_->on_order_update(*engine_.order(u->order));
        } else if (const auto* a = std::get_if<OrderAmended>(&e)) {
            hooks_->on_order_update(*engine_.order(a->order));
        }
    }
}

}  // namespace finforge
