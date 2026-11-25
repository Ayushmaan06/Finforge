#pragma once

// C++ strategy interface for test harnesses and synthetic agents (PLAN Phase 2 task 6). The Python
// SDK (Phase 5) wraps the same Context operations.

#include <cstdint>

#include "finforge/events.hpp"
#include "finforge/order.hpp"
#include "finforge/order_book.hpp"
#include "finforge/types.hpp"

namespace finforge {

class Ledger;

struct OrderRequest {
    InstrumentId instrument;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Tif tif = Tif::Gtc;
    Price price;
    Qty qty;
};

// What a strategy may do. Every action is delayed by the latency (EVT-3).
class Context {
  public:
    virtual ~Context() = default;

    [[nodiscard]] virtual Timestamp now() const = 0;
    [[nodiscard]] virtual OwnerId owner() const = 0;

    // The order arrives at now() + latency. Returns a client reference (per run, from 1) that comes
    // back in OwnOrderUpdate, so the strategy can match updates to its requests.
    virtual std::uint64_t submit(const OrderRequest& request) = 0;
    // Only the strategy's own orders; anything else is refused UNKNOWN_ORDER on arrival.
    virtual void cancel(OrderId id) = 0;
    virtual void amend(OrderId id, Qty new_qty, Price new_price) = 0;

    // Fires on_timer(tag) at `at`, which must be > now(). Throws std::invalid_argument otherwise.
    virtual void set_timer(Timestamp at, std::uint64_t tag) = 0;

    // Mode B: the live book (public information). nullptr in models without a book.
    [[nodiscard]] virtual const OrderBook* book(InstrumentId id) const = 0;

    // The strategy's own account (cash, positions, P&L), or nullptr without accounts.
    [[nodiscard]] virtual const Ledger* account() const = 0;
};

struct OwnOrderUpdate {
    std::uint64_t client_ref = 0;
    OrderUpdate update;
};

class Strategy {
  public:
    virtual ~Strategy() = default;

    virtual void on_start(Context&) {}
    virtual void on_timer(Context&, std::uint64_t /*tag*/) {}
    virtual void on_market_data(Context&, const MarketData&) {}
    // Status transitions of the strategy's own orders.
    virtual void on_order_update(Context&, const OwnOrderUpdate&) {}
    virtual void on_order_amended(Context&, const OrderAmended&) {}
    // Fills of the strategy's own orders (both sides of a self-trade arrive separately).
    virtual void on_fill(Context&, const Trade&) {}
    // A cancel or amend that the exchange refused (UNKNOWN_ORDER, ALREADY_TERMINAL, ...).
    virtual void on_action_refused(Context&, OrderId, RejectReason) {}
};

}  // namespace finforge
