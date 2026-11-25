#pragma once

// Execution models decide how orders fill (contracts.md §7). Mode B uses BookModel, which delegates
// to the Phase 1 order book (BKM-1). CandleModel and QuoteModel follow in Phase 4.

#include <cstddef>
#include <span>

#include "finforge/events.hpp"
#include "finforge/instrument.hpp"
#include "finforge/matching_engine.hpp"
#include "finforge/order.hpp"
#include "finforge/order_book.hpp"

namespace finforge {

class ExecutionModel {
  public:
    virtual ~ExecutionModel() = default;

    // Order entry. Each call replaces the events available from events().
    virtual OrderId submit(const NewOrder& order) = 0;
    virtual ActionResult cancel(OrderId id) = 0;
    virtual ActionResult amend(OrderId id, Qty new_qty, Price new_price, Timestamp ts) = 0;

    // Market data may produce fills in bar/quote models. Replaces events().
    virtual void on_market_data(const MarketData& md) = 0;

    // Events from the last call, in order.
    [[nodiscard]] virtual std::span<const BookEvent> events() const = 0;
    [[nodiscard]] virtual const Order* order(OrderId id) const = 0;
    [[nodiscard]] virtual std::size_t instrument_count() const = 0;
    // Live book, where the model has one (Mode B); nullptr otherwise.
    [[nodiscard]] virtual const OrderBook* book(InstrumentId) const { return nullptr; }
};

class BookModel final : public ExecutionModel {
  public:
    explicit BookModel(InstrumentRegistry registry) : engine_(std::move(registry)) {}

    OrderId submit(const NewOrder& order) override {
        md_ = false;
        return engine_.submit(order);
    }
    ActionResult cancel(OrderId id) override {
        md_ = false;
        return engine_.cancel(id);
    }
    ActionResult amend(OrderId id, Qty q, Price p, Timestamp ts) override {
        md_ = false;
        return engine_.amend(id, q, p, ts);
    }
    // External market data does not touch the simulated book in Mode B.
    void on_market_data(const MarketData&) override { md_ = true; }

    [[nodiscard]] std::span<const BookEvent> events() const override {
        if (md_) return {};
        return engine_.events();
    }
    [[nodiscard]] const Order* order(OrderId id) const override { return engine_.order(id); }
    [[nodiscard]] std::size_t instrument_count() const override {
        return engine_.registry().size();
    }
    [[nodiscard]] const OrderBook* book(InstrumentId id) const override {
        return id.value() < engine_.registry().size() ? &engine_.book(id) : nullptr;
    }

    [[nodiscard]] const MatchingEngine& engine() const { return engine_; }

  private:
    MatchingEngine engine_;
    bool md_ = false;  // last call was market data: no events
};

}  // namespace finforge
