#pragma once

// Orders, their lifecycle, and the events the matching engine emits (contracts.md §4, §5).

#include <cstdint>
#include <string_view>
#include <variant>

#include "finforge/types.hpp"

namespace finforge {

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Market, Limit };
enum class Tif : std::uint8_t { Gtc, Ioc, Fok };

enum class OrderStatus : std::uint8_t {
    New,
    Accepted,
    PartiallyFilled,
    Filled,     // terminal
    Cancelled,  // terminal
    Rejected,   // terminal
};

// ORD-4. `None` is not a reason; it marks "no rejection".
enum class RejectReason : std::uint8_t {
    None,
    InvalidQty,
    InvalidPrice,
    OffTick,
    OffLot,
    UnknownInstrument,
    InsufficientCash,
    PositionLimit,
    LeverageLimit,
    ShortNotAllowed,
    UnknownOrder,
    AlreadyTerminal,
};

// ORD-5. `None` marks "not cancelled".
enum class CancelReason : std::uint8_t {
    None,
    User,
    IocRemainder,
    FokUnfillable,
    MarketRemainder,
    EndOfRun,
};

std::string_view to_string(Side);
std::string_view to_string(OrderType);
std::string_view to_string(Tif);
std::string_view to_string(OrderStatus);
std::string_view to_string(RejectReason);
std::string_view to_string(CancelReason);

constexpr Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

constexpr bool is_terminal(OrderStatus s) {
    return s == OrderStatus::Filled || s == OrderStatus::Cancelled || s == OrderStatus::Rejected;
}

// ORD-1: the transitions in the lifecycle diagram, plus PARTIALLY_FILLED -> PARTIALLY_FILLED and
// ACCEPTED -> FILLED (one fill completes the order).
bool is_legal_transition(OrderStatus from, OrderStatus to);

struct Order {
    OrderId id;
    Seq seq;
    OwnerId owner;
    InstrumentId instrument;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Tif tif = Tif::Gtc;
    OrderStatus status = OrderStatus::New;
    RejectReason reject_reason = RejectReason::None;
    CancelReason cancel_reason = CancelReason::None;
    Price price;  // LIMIT only; Price{0} for MARKET
    Qty qty;
    Qty filled;
    Qty remaining;
    Qty cancelled;  // ORD-8: filled + remaining + cancelled == qty
    Timestamp submit_ts;

    // Intrusive FIFO links inside a price level; OrderId{0} = none. Owned by OrderBook.
    OrderId prev;
    OrderId next;
};

// Moves `o` to `to`. Trips FINFORGE_CHECK on an illegal transition (ORD-1).
void transition(Order& o, OrderStatus to);

// --- Events -------------------------------------------------------------------------------------

// One per status transition (ORD-2).
struct OrderUpdate {
    OrderId order;
    OrderStatus status = OrderStatus::New;
    Qty filled;
    Qty remaining;
    RejectReason reject_reason = RejectReason::None;
    CancelReason cancel_reason = CancelReason::None;

    friend bool operator==(const OrderUpdate&, const OrderUpdate&) = default;
};

// Quantity reduced in place, priority kept (ORD-7). Not a status transition.
struct OrderAmended {
    OrderId order;
    Qty qty;
    Qty remaining;

    friend bool operator==(const OrderAmended&, const OrderAmended&) = default;
};

// MAT-2: price is the resting order's price. MAT-6: both owners recorded.
struct Trade {
    TradeId id;
    InstrumentId instrument;
    Price price;
    Qty qty;
    OrderId aggressor;
    OrderId resting;
    OwnerId aggressor_owner;
    OwnerId resting_owner;
    Side aggressor_side = Side::Buy;
    Timestamp ts;

    friend bool operator==(const Trade&, const Trade&) = default;
};

// Emitted in this order for a submit: Accepted|Rejected; then per fill: Trade, resting update,
// aggressor update; then the aggressor's cancel, if any.
using BookEvent = std::variant<OrderUpdate, OrderAmended, Trade>;

}  // namespace finforge
