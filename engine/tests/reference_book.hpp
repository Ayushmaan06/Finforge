#pragma once

// ReferenceBook: a deliberately naive, obviously correct implementation of the same contract as
// MatchingEngine (contracts.md §4–§5). Resting orders sit in one flat, unordered vector that is
// scanned on every match. It shares no code with the engine apart from the plain data types, so
// agreement between the two is evidence that both are right (differential testing).

#include <algorithm>
#include <map>
#include <optional>
#include <vector>

#include "finforge/instrument.hpp"
#include "finforge/matching_engine.hpp"  // NewOrder, ActionResult
#include "finforge/order.hpp"
#include "finforge/order_book.hpp"  // LevelInfo

namespace finforge::test {

class ReferenceBook {
  public:
    explicit ReferenceBook(InstrumentRegistry reg) : reg_(std::move(reg)) {}

    OrderId submit(const NewOrder& in) {
        events_.clear();
        return do_submit(in);
    }

    ActionResult cancel(OrderId id) {
        events_.clear();
        Order* o = find(id);
        if (!o) return {RejectReason::UnknownOrder, {}};
        if (is_terminal(o->status)) return {RejectReason::AlreadyTerminal, {}};
        unrest(id);
        cancel_rest(id, CancelReason::User);
        return {};
    }

    ActionResult amend(OrderId id, Qty new_qty, Price new_price, Timestamp ts) {
        events_.clear();
        Order* o = find(id);
        if (!o) return {RejectReason::UnknownOrder, {}};
        if (is_terminal(o->status)) return {RejectReason::AlreadyTerminal, {}};
        if (!(new_qty > o->filled)) return {RejectReason::InvalidQty, {}};
        if (!(new_price > Price{0})) return {RejectReason::InvalidPrice, {}};
        if (new_price == o->price && new_qty == o->qty) return {};
        if (new_price == o->price && new_qty < o->qty) {
            o->remaining = o->remaining - (o->qty - new_qty);
            o->qty = new_qty;
            events_.push_back(OrderAmended{o->id, o->qty, o->remaining});
            return {};
        }
        NewOrder n{o->owner,  o->instrument,       o->side, o->type, o->tif,
                   new_price, new_qty - o->filled, ts};
        unrest(id);
        cancel_rest(id, CancelReason::User);
        return {RejectReason::None, do_submit(n)};
    }

    const std::vector<BookEvent>& events() const { return events_; }

    const Order* order(OrderId id) const {
        return (id.value() >= 1 && id.value() <= all_.size()) ? &all_[id.value() - 1] : nullptr;
    }

    std::optional<Price> best(InstrumentId inst, Side side) const {
        std::optional<Price> b;
        for (OrderId id : resting_) {
            const Order& o = all_[id.value() - 1];
            if (o.side != side || o.instrument != inst) continue;
            if (!b || (side == Side::Buy ? o.price > *b : o.price < *b)) b = o.price;
        }
        return b;
    }

    std::vector<LevelInfo> depth(InstrumentId inst, Side side, std::size_t n) const {
        std::map<std::int64_t, LevelInfo> levels;  // keyed by "goodness": -price for bids
        for (OrderId id : resting_) {
            const Order& o = all_[id.value() - 1];
            if (o.side != side || o.instrument != inst) continue;
            auto& l = levels[side == Side::Buy ? -o.price.value() : o.price.value()];
            l.price = o.price;
            l.qty = l.qty + o.remaining;
            ++l.orders;
        }
        std::vector<LevelInfo> out;
        for (const auto& [k, l] : levels) {
            if (out.size() == n) break;
            out.push_back(l);
        }
        return out;
    }

  private:
    Order* find(OrderId id) {
        return (id.value() >= 1 && id.value() <= all_.size()) ? &all_[id.value() - 1] : nullptr;
    }

    void update(const Order& o) {
        events_.push_back(
            OrderUpdate{o.id, o.status, o.filled, o.remaining, o.reject_reason, o.cancel_reason});
    }

    void cancel_rest(OrderId id, CancelReason r) {
        Order& o = all_[id.value() - 1];
        o.cancelled = o.cancelled + o.remaining;
        o.remaining = Qty{0};
        o.cancel_reason = r;
        o.status = OrderStatus::Cancelled;
        update(o);
    }

    void unrest(OrderId id) { std::erase(resting_, id); }

    bool crosses(const Order& aggr, const Order& rest) const {
        if (aggr.type == OrderType::Market) return true;
        return aggr.side == Side::Buy ? rest.price <= aggr.price : rest.price >= aggr.price;
    }

    // Best resting order the aggressor may trade with: best price, then lowest seq.
    std::optional<OrderId> best_match(const Order& aggr) const {
        std::optional<OrderId> best;
        for (OrderId id : resting_) {
            const Order& r = all_[id.value() - 1];
            if (r.side == aggr.side || r.instrument != aggr.instrument || !crosses(aggr, r)) {
                continue;
            }
            if (!best) {
                best = id;
                continue;
            }
            const Order& b = all_[best->value() - 1];
            const bool better_price =
                aggr.side == Side::Buy ? r.price < b.price : r.price > b.price;
            if (better_price || (r.price == b.price && r.seq < b.seq)) best = id;
        }
        return best;
    }

    OrderId do_submit(const NewOrder& in) {
        const OrderId id{all_.size() + 1};
        Order o;
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
        all_.push_back(o);
        Order& a = all_.back();  // all_ does not grow again in this call

        const Instrument* inst = reg_.find(in.instrument);
        RejectReason rej = RejectReason::None;
        if (!inst) {
            rej = RejectReason::UnknownInstrument;
        } else if (in.qty < inst->spec.min_qty) {
            rej = RejectReason::InvalidQty;
        } else if (in.type == OrderType::Market ? in.price != Price{0} : in.price <= Price{0}) {
            rej = RejectReason::InvalidPrice;
        }
        if (rej != RejectReason::None) {
            a.reject_reason = rej;
            a.status = OrderStatus::Rejected;
            update(a);
            return id;
        }
        a.remaining = a.qty;
        a.status = OrderStatus::Accepted;
        update(a);

        if (a.type == OrderType::Limit && a.tif == Tif::Fok) {
            Qty avail{0};
            for (OrderId rid : resting_) {
                const Order& r = all_[rid.value() - 1];
                if (r.side != a.side && r.instrument == a.instrument && crosses(a, r)) {
                    avail = avail + r.remaining;
                }
            }
            if (avail < a.qty) {
                cancel_rest(id, CancelReason::FokUnfillable);
                return id;
            }
        }

        while (a.remaining > Qty{0}) {
            const auto m = best_match(a);
            if (!m) break;
            Order& r = all_[m->value() - 1];
            const Qty q = std::min(a.remaining, r.remaining);
            events_.push_back(Trade{TradeId{next_trade_++}, a.instrument, r.price, q, a.id, r.id,
                                    a.owner, r.owner, a.side, a.submit_ts});
            r.filled = r.filled + q;
            r.remaining = r.remaining - q;
            r.status = r.remaining == Qty{0} ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
            if (r.remaining == Qty{0}) unrest(r.id);
            update(r);
            a.filled = a.filled + q;
            a.remaining = a.remaining - q;
            a.status = a.remaining == Qty{0} ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
            update(a);
        }

        if (a.remaining > Qty{0}) {
            if (a.type == OrderType::Market) {
                cancel_rest(id, CancelReason::MarketRemainder);
            } else if (a.tif == Tif::Ioc) {
                cancel_rest(id, CancelReason::IocRemainder);
            } else {
                resting_.push_back(id);
            }
        }
        return id;
    }

    InstrumentRegistry reg_;
    std::vector<Order> all_;
    std::vector<OrderId> resting_;
    std::vector<BookEvent> events_;
    std::uint64_t next_trade_ = 1;
};

}  // namespace finforge::test
