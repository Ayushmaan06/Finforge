#include "finforge/matching_engine.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "finforge/assert.hpp"

namespace finforge {

template <class F>
decltype(auto) MatchingEngine::with_levels(OrderBook& book, Side side, F&& f) {
    return side == Side::Buy ? f(book.bids_) : f(book.asks_);
}

MatchingEngine::MatchingEngine(InstrumentRegistry registry) : registry_(std::move(registry)) {
    books_.reserve(registry_.size());
    for (std::size_t i = 0; i < registry_.size(); ++i) {
        books_.emplace_back(InstrumentId{static_cast<InstrumentId::rep>(i)});
    }
}

const Order* MatchingEngine::order(OrderId id) const {
    if (id.value() == 0 || id.value() > orders_.size()) return nullptr;
    return &at(id);
}

const OrderBook& MatchingEngine::book(InstrumentId id) const {
    FINFORGE_CHECK(id.value() < books_.size(), "unknown instrument");
    return books_[id.value()];
}

// --- Operations ---------------------------------------------------------------------------------

OrderId MatchingEngine::submit(const NewOrder& order) {
    events_.clear();
    const OrderId id = submit_impl(order);
    after_operation();
    return id;
}

ActionResult MatchingEngine::cancel(OrderId id) {
    events_.clear();
    if (id.value() == 0 || id.value() > orders_.size()) return {RejectReason::UnknownOrder, {}};
    Order& o = at(id);
    if (is_terminal(o.status)) return {RejectReason::AlreadyTerminal, {}};
    with_levels(books_[o.instrument.value()], o.side, [&](auto& levels) { unlink(levels, o); });
    cancel_remaining(o, CancelReason::User);
    after_operation();
    return {};
}

void MatchingEngine::cancel_all(CancelReason reason) {
    events_.clear();
    for (Order& o : orders_) {
        if (is_terminal(o.status)) continue;
        with_levels(books_[o.instrument.value()], o.side, [&](auto& levels) { unlink(levels, o); });
        cancel_remaining(o, reason);
    }
    after_operation();
}

ActionResult MatchingEngine::amend(OrderId id, Qty new_qty, Price new_price, Timestamp ts,
                                   std::optional<Timestamp> sent) {
    events_.clear();
    if (id.value() == 0 || id.value() > orders_.size()) return {RejectReason::UnknownOrder, {}};
    Order& o = at(id);
    if (is_terminal(o.status)) return {RejectReason::AlreadyTerminal, {}};
    if (new_qty <= o.filled) return {RejectReason::InvalidQty, {}};
    if (new_price <= Price{0}) return {RejectReason::InvalidPrice, {}};

    OrderBook& book = books_[o.instrument.value()];
    if (new_price == o.price && new_qty <= o.qty) {
        if (new_qty == o.qty) return {};  // nothing to change
        // ORD-7: reduce in place; the order keeps its place in the queue.
        const Qty delta = o.qty - new_qty;
        with_levels(book, o.side, [&](auto& levels) {
            auto it = levels.find(o.price);
            FINFORGE_CHECK(it != levels.end(), "resting order has no level");
            it->second.total -= delta;
        });
        o.qty = new_qty;
        o.remaining -= delta;
        events_.push_back(OrderAmended{o.id, o.qty, o.remaining});
        after_operation();
        return {};
    }

    // ORD-7: cancel-and-replace. Copy what the new order needs first: submit_impl appends to
    // orders_, which may reallocate and invalidate `o`.
    const NewOrder replacement{o.owner,   o.instrument,       o.side, o.type, o.tif,
                               new_price, new_qty - o.filled, ts,     sent};
    with_levels(book, o.side, [&](auto& levels) { unlink(levels, o); });
    cancel_remaining(o, CancelReason::User);
    const OrderId new_id = submit_impl(replacement);
    after_operation();
    return {RejectReason::None, new_id};
}

OrderId MatchingEngine::submit_impl(const NewOrder& in) {
    const OrderId id{orders_.size() + 1};  // NUM-7: dense, from 1
    Order& o = orders_.emplace_back();
    o.id = id;
    o.seq = Seq{id.value()};
    o.owner = in.owner;
    o.instrument = in.instrument;
    o.side = in.side;
    o.type = in.type;
    o.tif = in.tif;
    o.price = in.price;
    o.qty = in.qty;
    o.submit_ts = in.ts;
    o.sent_ts = in.sent.value_or(in.ts);

    RejectReason r = validate_new_order(registry_, in);
    if (r == RejectReason::None && gate_) r = gate_(in);
    if (r != RejectReason::None) {
        o.reject_reason = r;
        set_status(o, OrderStatus::Rejected);
        return id;
    }
    o.remaining = o.qty;
    set_status(o, OrderStatus::Accepted);

    OrderBook& book = books_[o.instrument.value()];
    const bool fok = o.type == OrderType::Limit && o.tif == Tif::Fok;
    const bool unfillable = with_levels(book, opposite(o.side), [&](auto& levels) {
        // MAT-4: FOK is decided before any fill.
        if (fok && available(o, levels) < o.qty) return true;
        match(o, levels);
        return false;
    });

    if (unfillable) {
        cancel_remaining(o, CancelReason::FokUnfillable);
    } else if (o.remaining > Qty{0}) {
        if (o.type == OrderType::Market) {
            cancel_remaining(o, CancelReason::MarketRemainder);  // ORD-6: never rests
        } else if (o.tif == Tif::Ioc) {
            cancel_remaining(o, CancelReason::IocRemainder);
        } else {
            FINFORGE_CHECK(o.tif == Tif::Gtc, "FOK order with a remainder after matching");
            with_levels(book, o.side, [&](auto& levels) { link(levels, o); });
        }
    }
    return id;
}

RejectReason validate_new_order(const InstrumentRegistry& registry, const NewOrder& in) {
    const Instrument* inst = registry.find(in.instrument);
    if (inst == nullptr) return RejectReason::UnknownInstrument;       // INS-3
    if (in.qty < inst->spec.min_qty) return RejectReason::InvalidQty;  // NUM-6, INS-3
    if (in.type == OrderType::Market) {
        if (in.price != Price{0}) return RejectReason::InvalidPrice;  // ORD-6
    } else if (in.price <= Price{0}) {
        return RejectReason::InvalidPrice;  // NUM-6
    }
    return RejectReason::None;
}

// --- Matching -----------------------------------------------------------------------------------

template <class Levels>
Qty MatchingEngine::available(const Order& aggressor, const Levels& levels) const {
    const auto worse = levels.key_comp();
    Qty sum{0};
    for (const auto& [price, level] : levels) {
        if (aggressor.type == OrderType::Limit && worse(aggressor.price, price)) break;
        sum += level.total;
        if (sum >= aggressor.qty) break;
    }
    return sum;
}

template <class Levels>
void MatchingEngine::match(Order& aggressor, Levels& levels) {
    // References into orders_ stay valid here: nothing is appended while matching.
    const auto worse = levels.key_comp();
    while (aggressor.remaining > Qty{0} && !levels.empty()) {
        auto it = levels.begin();
        if (aggressor.type == OrderType::Limit && worse(aggressor.price, it->first)) break;
        Level& level = it->second;
        while (aggressor.remaining > Qty{0} && level.count > 0) {
            Order& resting = at(level.head);
            const Qty q = std::min(aggressor.remaining, resting.remaining);
            // MAT-7
            FINFORGE_CHECK(q > Qty{0}, "fill quantity must be positive");
            FINFORGE_CHECK(resting.side != aggressor.side, "fill on the same side");
            FINFORGE_CHECK(resting.instrument == aggressor.instrument, "fill across instruments");
            FINFORGE_CHECK(!is_terminal(resting.status), "fill against a terminal order");

            events_.push_back(Trade{TradeId{next_trade_id_++}, aggressor.instrument,
                                    resting.price,  // MAT-2
                                    q, aggressor.id, resting.id, aggressor.owner, resting.owner,
                                    aggressor.side, aggressor.submit_ts});
            level.total -= q;
            resting.filled += q;
            resting.remaining -= q;
            if (resting.remaining == Qty{0}) {
                // Pop the head (MAT-5: fully filled resting orders are removed).
                level.head = resting.next;
                if (level.head.value() != 0) {
                    at(level.head).prev = OrderId{};
                } else {
                    level.tail = OrderId{};
                }
                resting.next = OrderId{};
                --level.count;
                set_status(resting, OrderStatus::Filled);
            } else {
                set_status(resting, OrderStatus::PartiallyFilled);
            }
            aggressor.filled += q;
            aggressor.remaining -= q;
            set_status(aggressor, aggressor.remaining == Qty{0} ? OrderStatus::Filled
                                                                : OrderStatus::PartiallyFilled);
        }
        if (level.count == 0) levels.erase(it);  // MAT-8
    }
}

template <class Levels>
void MatchingEngine::link(Levels& levels, Order& o) {
    Level& level = levels[o.price];
    o.prev = level.tail;
    o.next = OrderId{};
    if (level.tail.value() != 0) {
        at(level.tail).next = o.id;
    } else {
        level.head = o.id;
    }
    level.tail = o.id;
    ++level.count;
    level.total += o.remaining;
}

template <class Levels>
void MatchingEngine::unlink(Levels& levels, Order& o) {
    auto it = levels.find(o.price);
    FINFORGE_CHECK(it != levels.end(), "resting order has no level");
    Level& level = it->second;
    if (o.prev.value() != 0) {
        at(o.prev).next = o.next;
    } else {
        level.head = o.next;
    }
    if (o.next.value() != 0) {
        at(o.next).prev = o.prev;
    } else {
        level.tail = o.prev;
    }
    o.prev = OrderId{};
    o.next = OrderId{};
    --level.count;
    level.total -= o.remaining;
    if (level.count == 0) levels.erase(it);  // MAT-8
}

// --- Lifecycle ----------------------------------------------------------------------------------

void MatchingEngine::set_status(Order& o, OrderStatus to) {
    const OrderStatus from = o.status;
    transition(o, to);  // ORD-1
    if (to != OrderStatus::Rejected) {
        FINFORGE_CHECK(o.filled + o.remaining + o.cancelled == o.qty,
                       "quantity not conserved (ORD-8)");
    }
    if (to == OrderStatus::Accepted) ++open_orders_;
    if (is_terminal(to) && from != OrderStatus::New) --open_orders_;
    // ORD-2: exactly one event per transition.
    events_.push_back(
        OrderUpdate{o.id, o.status, o.filled, o.remaining, o.reject_reason, o.cancel_reason});
}

void MatchingEngine::cancel_remaining(Order& o, CancelReason reason) {
    o.cancelled += o.remaining;
    o.remaining = Qty{0};
    o.cancel_reason = reason;
    set_status(o, OrderStatus::Cancelled);
}

// --- Invariants ---------------------------------------------------------------------------------

void MatchingEngine::after_operation() const {
#ifdef FINFORGE_EXPENSIVE_CHECKS
    check_invariants();
#endif
}

template <class Levels>
void MatchingEngine::check_side(const OrderBook& book, const Levels& levels, Side side,
                                std::size_t& resting) const {
    for (const auto& [price, level] : levels) {
        FINFORGE_CHECK(level.count > 0 && level.total > Qty{0}, "empty price level (MAT-8)");
        Qty sum{0};
        std::uint32_t n = 0;
        OrderId prev{};
        for (OrderId id = level.head; id.value() != 0; id = at(id).next) {
            const Order& o = at(id);
            FINFORGE_CHECK(o.prev == prev, "broken level links");
            FINFORGE_CHECK(
                o.status == OrderStatus::Accepted || o.status == OrderStatus::PartiallyFilled,
                "non-open order resting");
            FINFORGE_CHECK(o.type == OrderType::Limit && o.tif == Tif::Gtc,
                           "only GTC limit orders rest (ORD-6, MAT-4)");
            FINFORGE_CHECK(o.side == side && o.price == price && o.instrument == book.instrument(),
                           "order in the wrong level");
            FINFORGE_CHECK(o.remaining > Qty{0}, "resting order with nothing left (MAT-5)");
            FINFORGE_CHECK(o.filled + o.remaining + o.cancelled == o.qty, "ORD-8");
            FINFORGE_CHECK(prev.value() == 0 || at(prev).seq < o.seq, "FIFO order broken (MAT-1)");
            sum += o.remaining;
            ++n;
            prev = id;
        }
        FINFORGE_CHECK(level.tail == prev, "level tail mismatch");
        FINFORGE_CHECK(n == level.count && sum == level.total, "level totals mismatch");
        resting += n;
    }
}

void MatchingEngine::check_invariants() const {
    std::size_t resting = 0;
    for (const OrderBook& book : books_) {
        check_side(book, book.bids_, Side::Buy, resting);
        check_side(book, book.asks_, Side::Sell, resting);
        if (!book.bids_.empty() && !book.asks_.empty()) {
            FINFORGE_CHECK(book.bids_.begin()->first < book.asks_.begin()->first,
                           "book is crossed (MAT-5)");
        }
    }
    FINFORGE_CHECK(resting == open_orders_, "open orders not all resting");
}

}  // namespace finforge
