#pragma once

// Order entry for Mode B: validation, id assignment, matching and the event stream
// (contracts.md §4, §5). One OrderBook per instrument; ids are per run and dense (NUM-7).

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "finforge/instrument.hpp"
#include "finforge/order.hpp"
#include "finforge/order_book.hpp"
#include "finforge/types.hpp"

namespace finforge {

struct NewOrder {
    OwnerId owner;
    InstrumentId instrument;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Tif tif = Tif::Gtc;  // ignored for MARKET (ORD-6)
    Price price;         // must be Price{0} for MARKET
    Qty qty;
    Timestamp ts;                     // arrival at the execution model
    std::optional<Timestamp> sent{};  // when the strategy sent it; unset = same as ts
};

// Basic order validation shared by every model: instrument, quantity, price (INS-3, NUM-6, ORD-6).
RejectReason validate_new_order(const InstrumentRegistry& registry, const NewOrder& order);

// Extra pre-trade check run after validate_new_order passes (ACC-9: cash, limits, leverage).
using OrderGate = std::function<RejectReason(const NewOrder&)>;

// Result of cancel/amend. `reason == None` means the action was applied.
struct ActionResult {
    RejectReason reason = RejectReason::None;
    OrderId new_order;  // amend by cancel-and-replace only

    [[nodiscard]] bool ok() const { return reason == RejectReason::None; }
};

class MatchingEngine {
  public:
    explicit MatchingEngine(InstrumentRegistry registry);

    // Every order gets an id, including rejected ones. Events of this call: events().
    OrderId submit(const NewOrder& order);

    // Cancels a resting order (CANCELLED, reason USER).
    ActionResult cancel(OrderId id);

    // ORD-7. `new_qty` is the new total quantity (including what is already filled).
    // - same price, filled < new_qty < qty: reduce in place, priority kept (OrderAmended event);
    // - same price and qty: no-op;
    // - otherwise cancel-and-replace: old order CANCELLED (USER), a new order for
    //   new_qty - filled at new_price is submitted with a new id and seq.
    ActionResult amend(OrderId id, Qty new_qty, Price new_price, Timestamp ts,
                       std::optional<Timestamp> sent = std::nullopt);

    // Cancels every open order with `reason` (OP-3: END_OF_RUN). Events: events().
    void cancel_all(CancelReason reason);

    // Runs on every new order, including amend replacements; an empty gate accepts everything.
    void set_gate(OrderGate gate) { gate_ = std::move(gate); }

    // Events emitted by the last submit/cancel/amend, in order.
    [[nodiscard]] std::span<const BookEvent> events() const { return events_; }

    // nullptr for an unknown id. Terminal orders stay queryable.
    [[nodiscard]] const Order* order(OrderId id) const;
    [[nodiscard]] const OrderBook& book(InstrumentId id) const;
    [[nodiscard]] const InstrumentRegistry& registry() const { return registry_; }
    [[nodiscard]] std::size_t order_count() const { return orders_.size(); }
    [[nodiscard]] std::size_t open_order_count() const { return open_orders_; }
    [[nodiscard]] std::uint64_t trade_count() const { return next_trade_id_ - 1; }

    // MAT-1, MAT-5, MAT-8, ORD-8 over every book. O(resting orders). Trips FINFORGE_CHECK.
    // Runs after every operation when FINFORGE_EXPENSIVE_CHECKS is defined (Debug builds).
    void check_invariants() const;

  private:
    Order& at(OrderId id) { return orders_[id.value() - 1]; }
    const Order& at(OrderId id) const { return orders_[id.value() - 1]; }

    OrderId submit_impl(const NewOrder& order);
    void set_status(Order& o, OrderStatus to);
    void cancel_remaining(Order& o, CancelReason reason);

    // Calls f(bids) or f(asks): one templated algorithm serves both sides.
    template <class F>
    static decltype(auto) with_levels(OrderBook& book, Side side, F&& f);
    template <class Levels>
    void match(Order& aggressor, Levels& levels);
    template <class Levels>
    Qty available(const Order& aggressor, const Levels& levels) const;
    template <class Levels>
    void link(Levels& levels, Order& o);
    template <class Levels>
    void unlink(Levels& levels, Order& o);
    template <class Levels>
    void check_side(const OrderBook& book, const Levels& levels, Side side,
                    std::size_t& resting) const;

    void after_operation() const;

    InstrumentRegistry registry_;
    OrderGate gate_;
    std::vector<OrderBook> books_;
    std::vector<Order> orders_;  // index = id - 1
    std::vector<BookEvent> events_;
    std::uint64_t next_trade_id_ = 1;
    std::size_t open_orders_ = 0;  // non-terminal orders == resting orders between operations
};

}  // namespace finforge
