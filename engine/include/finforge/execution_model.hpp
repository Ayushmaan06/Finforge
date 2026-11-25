#pragma once

// Execution models decide how orders fill (contracts.md §7). Mode B uses BookModel, which delegates
// to the Phase 1 order book (BKM-1). Mode A uses CandleModel and QuoteModel (mode_a.hpp).
//
// Models report to accounts through AccountHooks (accounts.hpp implements them). Mode A models call
// the hooks while they fill, so a second fill in the same event already sees the first one's cash.

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#include "finforge/events.hpp"
#include "finforge/instrument.hpp"
#include "finforge/matching_engine.hpp"
#include "finforge/order.hpp"
#include "finforge/order_book.hpp"

namespace finforge {

class AccountHooks {
  public:
    virtual ~AccountHooks() = default;

    // ACC-9 checks (cash, position limit, shorts, leverage), run after the model's own validation
    // (instrument, quantity, price). `reference` is the price the order is checked at: the limit
    // for LIMIT orders, the mark adjusted by slippage for MARKET orders. None = accept.
    virtual RejectReason pre_trade(const NewOrder& order, Price reference) = 0;

    // OP-1: how many of `wanted` lots of `order` (a buy) the account can pay for at `price`,
    // fee included. Returns `wanted` for sells and for owners without an account.
    virtual Qty affordable(const Order& order, Price price, Qty wanted) = 0;

    // Every fill of `order`, as it happens. Called before the order's status update.
    virtual void on_fill(const Order& order, const Trade& trade, Liquidity liquidity) = 0;

    // After every status change or amendment of an order (reservations, open-order tracking).
    virtual void on_order_update(const Order& order) = 0;
};

class ExecutionModel {
  public:
    virtual ~ExecutionModel() = default;

    // D4: every result records which model produced it.
    [[nodiscard]] virtual std::string_view name() const = 0;

    // Order entry. Each call replaces the events available from events().
    virtual OrderId submit(const NewOrder& order) = 0;
    virtual ActionResult cancel(OrderId id) = 0;
    // `sent` is when the strategy sent the amend (a replacement order's sent time, CND-1).
    virtual ActionResult amend(OrderId id, Qty new_qty, Price new_price, Timestamp ts,
                               Timestamp sent) = 0;
    // Cancels every open order (OP-3: END_OF_RUN). Replaces events().
    virtual void cancel_all(CancelReason reason) = 0;

    // Market data may produce fills in bar/quote models. Replaces events().
    virtual void on_market_data(const MarketData& md) = 0;

    // Events from the last call, in order.
    [[nodiscard]] virtual std::span<const BookEvent> events() const = 0;
    [[nodiscard]] virtual const Order* order(OrderId id) const = 0;
    // Orders submitted so far, including rejected ones; ids are 1..order_count() (NUM-7).
    [[nodiscard]] virtual std::size_t order_count() const = 0;
    [[nodiscard]] virtual const InstrumentRegistry& registry() const = 0;
    [[nodiscard]] std::size_t instrument_count() const { return registry().size(); }
    // Live book, where the model has one (Mode B); nullptr otherwise.
    [[nodiscard]] virtual const OrderBook* book(InstrumentId) const { return nullptr; }
    // ACC-6 mark price: last close (Candle), floor(mid) of the last quote (Quote) or of the book
    // (Book). nullopt before any data.
    [[nodiscard]] virtual std::optional<Price> mark(InstrumentId id) const = 0;

    void set_account_hooks(AccountHooks* hooks) {
        hooks_ = hooks;
        on_hooks_changed();
    }
    [[nodiscard]] AccountHooks* account_hooks() const { return hooks_; }

  protected:
    virtual void on_hooks_changed() {}
    AccountHooks* hooks_ = nullptr;
};

class BookModel final : public ExecutionModel {
  public:
    explicit BookModel(InstrumentRegistry registry) : engine_(std::move(registry)) {}

    [[nodiscard]] std::string_view name() const override { return "book"; }

    OrderId submit(const NewOrder& order) override {
        md_ = false;
        const OrderId id = engine_.submit(order);
        report();
        return id;
    }
    ActionResult cancel(OrderId id) override {
        md_ = false;
        const ActionResult r = engine_.cancel(id);
        report();
        return r;
    }
    ActionResult amend(OrderId id, Qty q, Price p, Timestamp ts, Timestamp sent) override {
        md_ = false;
        const ActionResult r = engine_.amend(id, q, p, ts, sent);
        report();
        return r;
    }
    void cancel_all(CancelReason reason) override {
        md_ = false;
        engine_.cancel_all(reason);
        report();
    }
    // External market data does not touch the simulated book in Mode B.
    void on_market_data(const MarketData&) override { md_ = true; }

    [[nodiscard]] std::span<const BookEvent> events() const override {
        if (md_) return {};
        return engine_.events();
    }
    [[nodiscard]] const Order* order(OrderId id) const override { return engine_.order(id); }
    [[nodiscard]] std::size_t order_count() const override { return engine_.order_count(); }
    [[nodiscard]] const InstrumentRegistry& registry() const override { return engine_.registry(); }
    [[nodiscard]] const OrderBook* book(InstrumentId id) const override {
        return id.value() < engine_.registry().size() ? &engine_.book(id) : nullptr;
    }
    [[nodiscard]] std::optional<Price> mark(InstrumentId id) const override;

    [[nodiscard]] const MatchingEngine& engine() const { return engine_; }

  private:
    void on_hooks_changed() override;
    // Passes the last operation's fills and status changes to the account hooks.
    void report();

    MatchingEngine engine_;
    bool md_ = false;  // last call was market data: no events
};

}  // namespace finforge
