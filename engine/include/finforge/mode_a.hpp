#pragma once

// Mode A execution models (contracts.md §7.1, §7.2, §8): orders fill against historical bars or
// L1 quotes instead of a simulated book. Both share WorkingOrderModel for ids, validation, the
// lifecycle and cancel/amend; they differ only in how market data fills working orders.

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "finforge/execution_model.hpp"

namespace finforge {

class WorkingOrderModel : public ExecutionModel {
  public:
    explicit WorkingOrderModel(InstrumentRegistry registry);

    OrderId submit(const NewOrder& order) override;
    ActionResult cancel(OrderId id) override;
    // ORD-7: reduce in place (OrderAmended) or cancel-and-replace (new id, seq and sent time).
    ActionResult amend(OrderId id, Qty new_qty, Price new_price, Timestamp ts,
                       Timestamp sent) override;
    void cancel_all(CancelReason reason) override;

    [[nodiscard]] std::span<const BookEvent> events() const override { return events_; }
    [[nodiscard]] const Order* order(OrderId id) const override;
    [[nodiscard]] std::size_t order_count() const override { return orders_.size(); }
    [[nodiscard]] const InstrumentRegistry& registry() const override { return registry_; }
    [[nodiscard]] std::optional<Price> mark(InstrumentId id) const override;

    [[nodiscard]] std::size_t open_order_count() const;
    [[nodiscard]] std::uint64_t trade_count() const { return next_trade_ - 1; }

  protected:
    // The price ACC-9 checks a MARKET order at (contracts.md §9); LIMIT orders use their limit.
    [[nodiscard]] virtual Price market_reference(const NewOrder& order) const = 0;

    Order& at(OrderId id) { return orders_[id.value() - 1]; }
    // Working (non-terminal) orders of one instrument, in seq order.
    std::vector<OrderId>& working(InstrumentId id) { return working_[id.value()]; }
    void compact(InstrumentId id);  // drops terminal orders from working(id)

    // The market a fill is priced against (Trade::ref_bid / ref_ask).
    struct Touch {
        Price bid;
        Price ask;
    };

    // Fills `qty` of `o` at `price`: Trade, then the order's status update (hooks in between).
    void fill(Order& o, Price price, Qty qty, Timestamp ts, Touch touch);
    void cancel_remaining(Order& o, CancelReason reason);
    // OP-1: lots of `wanted` a buy can pay for at `price` (all of them without accounts).
    Qty affordable(const Order& o, Price price, Qty wanted);
    // One chance for `o` to trade `available` lots at `price` (0 = the market does not cross):
    // FOK all-or-nothing, the OP-1 cash cap, the fill, then MARKET / IOC remainders cancelled
    // (GTC keeps working). Returns the lots filled.
    Qty execute(Order& o, Price price, Qty available, Timestamp ts, Touch touch);

    std::vector<std::optional<Price>> marks_;  // ACC-6, per instrument
    std::vector<BookEvent> events_;

  private:
    OrderId submit_impl(const NewOrder& order);
    void set_status(Order& o, OrderStatus to);

    InstrumentRegistry registry_;
    std::vector<Order> orders_;  // index = id - 1
    std::vector<std::vector<OrderId>> working_;
    std::uint64_t next_trade_ = 1;
};

// --- Slippage (SLP-1, SLP-2) --------------------------------------------------------------------

enum class SlippageKind : std::uint8_t {
    None,
    FixedBps,    // price × (1 ± rate)
    HalfSpread,  // price × (1 ± rate / 2), rate = the full spread
};

struct Slippage {
    SlippageKind kind = SlippageKind::None;
    Rate rate{0};  // 10 bps = Rate{1000}

    // Adverse to the order and rounded to the tick grid against it (NUM-5): buys round up, sells
    // down. A sell never goes below 1 tick.
    [[nodiscard]] Price apply(Price price, Side side) const;
};

// --- CandleModel (§7.1) -------------------------------------------------------------------------

struct CandleConfig {
    Slippage slippage{};
    // CND-5 volume cap, off when 0: fill ≤ floor(participation × volume) lots per owner, bar and
    // instrument, shared by the owner's orders in seq order.
    Rate participation{0};
};

class CandleModel final : public WorkingOrderModel {
  public:
    explicit CandleModel(InstrumentRegistry registry, CandleConfig config = {});

    [[nodiscard]] std::string_view name() const override { return "candle"; }
    // Bars fill working orders; quotes and trade prints are ignored.
    void on_market_data(const MarketData& md) override;
    [[nodiscard]] const CandleConfig& config() const { return config_; }

  protected:
    [[nodiscard]] Price market_reference(const NewOrder& order) const override;

  private:
    CandleConfig config_;
};

// --- QuoteModel (§7.2) --------------------------------------------------------------------------

class QuoteModel final : public WorkingOrderModel {
  public:
    explicit QuoteModel(InstrumentRegistry registry);

    [[nodiscard]] std::string_view name() const override { return "quote"; }
    // Quotes fill working orders; bars and trade prints are ignored.
    void on_market_data(const MarketData& md) override;

  protected:
    [[nodiscard]] Price market_reference(const NewOrder& order) const override;
};

}  // namespace finforge
