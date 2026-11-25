#pragma once

// Simulation events and their total order (contracts.md §6).

#include <compare>
#include <cstdint>
#include <variant>

#include "finforge/matching_engine.hpp"  // NewOrder
#include "finforge/order.hpp"
#include "finforge/types.hpp"

namespace finforge {

// EVT-1: at equal timestamps, exchange actions run before market data, which runs before timers.
enum class EventClass : std::uint8_t {
    Exchange = 0,    // order arrivals, cancels, amends
    MarketData = 1,  // bars, quotes, trade prints
    Timer = 2,       // strategy timers, snapshots
};

// EVT-1: (ts, class, instrument_id, seq). seq is a per-run insertion counter, so the key is a total
// order and equal keys never occur.
struct EventKey {
    Timestamp ts;
    EventClass cls = EventClass::Exchange;
    InstrumentId instrument;
    Seq seq;

    friend constexpr bool operator==(const EventKey&, const EventKey&) = default;
    friend constexpr auto operator<=>(const EventKey&, const EventKey&) = default;
};

// --- Market data (class 1). Produced by the data pipeline from Phase 3 on. ----------------------

// TIME-2: labelled by open time, emitted at close_time.
struct Bar {
    InstrumentId instrument;
    Timestamp open_time;
    Timestamp close_time;
    Price open;
    Price high;
    Price low;
    Price close;
    Qty volume;

    friend bool operator==(const Bar&, const Bar&) = default;
};

struct Quote {
    InstrumentId instrument;
    Timestamp ts;
    Price bid;
    Qty bid_qty;
    Price ask;
    Qty ask_qty;

    friend bool operator==(const Quote&, const Quote&) = default;
};

struct TradePrint {
    InstrumentId instrument;
    Timestamp ts;
    Price price;
    Qty qty;
    Side aggressor_side = Side::Buy;

    friend bool operator==(const TradePrint&, const TradePrint&) = default;
};

using MarketData = std::variant<Bar, Quote, TradePrint>;

// Time at which a market data event is emitted (TIME-2, TIME-4).
Timestamp emit_time(const MarketData& md);
InstrumentId instrument_of(const MarketData& md);

// --- Exchange actions (class 0) -----------------------------------------------------------------

struct SubmitAction {
    OwnerId owner;
    std::uint64_t client_ref = 0;  // the strategy's handle, returned by Context::submit
    NewOrder order;                // order.ts is set to the arrival time when processed
};

struct CancelAction {
    OwnerId owner;
    OrderId order;
};

struct AmendAction {
    OwnerId owner;
    OrderId order;
    Qty qty;
    Price price;
};

// --- Timers (class 2) ---------------------------------------------------------------------------

struct TimerEvent {
    OwnerId owner;
    std::uint64_t tag = 0;
};

// Records the top of every book in the journal.
struct SnapshotEvent {};

using Event = std::variant<SubmitAction, CancelAction, AmendAction, Bar, Quote, TradePrint,
                           TimerEvent, SnapshotEvent>;

EventClass class_of(const Event& e);

struct QueuedEvent {
    EventKey key;
    Event event;
};

}  // namespace finforge
