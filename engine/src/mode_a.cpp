#include "finforge/mode_a.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <utility>
#include <variant>

#include "finforge/assert.hpp"

namespace finforge {

// --- WorkingOrderModel --------------------------------------------------------------------------

WorkingOrderModel::WorkingOrderModel(InstrumentRegistry registry)
    : marks_(registry.size()), registry_(std::move(registry)), working_(registry_.size()) {}

const Order* WorkingOrderModel::order(OrderId id) const {
    if (id.value() == 0 || id.value() > orders_.size()) return nullptr;
    return &orders_[id.value() - 1];
}

std::optional<Price> WorkingOrderModel::mark(InstrumentId id) const {
    return id.value() < marks_.size() ? marks_[id.value()] : std::nullopt;
}

std::size_t WorkingOrderModel::open_order_count() const {
    std::size_t n = 0;
    for (const auto& w : working_) n += w.size();
    return n;
}

OrderId WorkingOrderModel::submit(const NewOrder& order) {
    events_.clear();
    return submit_impl(order);
}

OrderId WorkingOrderModel::submit_impl(const NewOrder& in) {
    const OrderId id{orders_.size() + 1};  // NUM-7
    {
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
    }
    RejectReason r = validate_new_order(registry_, in);
    if (r == RejectReason::None && hooks_ != nullptr) {
        // ACC-9: the hooks see the order only after the model's own checks passed.
        r = hooks_->pre_trade(in, in.type == OrderType::Limit ? in.price : market_reference(in));
    }
    Order& o = at(id);
    if (r != RejectReason::None) {
        o.reject_reason = r;
        set_status(o, OrderStatus::Rejected);
        return id;
    }
    o.remaining = o.qty;
    set_status(o, OrderStatus::Accepted);
    working(o.instrument).push_back(id);
    return id;
}

ActionResult WorkingOrderModel::cancel(OrderId id) {
    events_.clear();
    if (order(id) == nullptr) return {RejectReason::UnknownOrder, {}};
    Order& o = at(id);
    if (is_terminal(o.status)) return {RejectReason::AlreadyTerminal, {}};
    cancel_remaining(o, CancelReason::User);
    compact(o.instrument);
    return {};
}

ActionResult WorkingOrderModel::amend(OrderId id, Qty new_qty, Price new_price, Timestamp ts,
                                      Timestamp sent) {
    events_.clear();
    if (order(id) == nullptr) return {RejectReason::UnknownOrder, {}};
    Order& o = at(id);
    if (is_terminal(o.status)) return {RejectReason::AlreadyTerminal, {}};
    if (new_qty <= o.filled) return {RejectReason::InvalidQty, {}};
    // ORD-7: a waiting MARKET order (Candle) keeps price 0; any other price is refused (ORD-6).
    const bool bad_price =
        o.type == OrderType::Market ? new_price != Price{0} : new_price <= Price{0};
    if (bad_price) return {RejectReason::InvalidPrice, {}};

    if (new_price == o.price && new_qty <= o.qty) {
        if (new_qty == o.qty) return {};
        const Qty delta = o.qty - new_qty;  // ORD-7: reduce in place
        o.qty = new_qty;
        o.remaining -= delta;
        events_.push_back(OrderAmended{o.id, o.qty, o.remaining});
        if (hooks_ != nullptr) hooks_->on_order_update(o);
        return {};
    }
    // Copy before submit_impl: it appends to orders_, which may invalidate `o`.
    const NewOrder replacement{o.owner,   o.instrument,       o.side, o.type, o.tif,
                               new_price, new_qty - o.filled, ts,     sent};
    const InstrumentId instrument = o.instrument;
    cancel_remaining(o, CancelReason::User);
    compact(instrument);
    const OrderId new_id = submit_impl(replacement);
    return {RejectReason::None, new_id};
}

void WorkingOrderModel::cancel_all(CancelReason reason) {
    events_.clear();
    for (auto& w : working_) {
        for (const OrderId id : w) {
            Order& o = at(id);
            if (!is_terminal(o.status)) cancel_remaining(o, reason);
        }
        w.clear();
    }
}

void WorkingOrderModel::compact(InstrumentId id) {
    auto& w = working(id);
    std::erase_if(w, [&](OrderId oid) { return is_terminal(at(oid).status); });
}

void WorkingOrderModel::set_status(Order& o, OrderStatus to) {
    transition(o, to);  // ORD-1
    if (to != OrderStatus::Rejected) {
        FINFORGE_CHECK(o.filled + o.remaining + o.cancelled == o.qty,
                       "quantity not conserved (ORD-8)");
    }
    events_.push_back(
        OrderUpdate{o.id, o.status, o.filled, o.remaining, o.reject_reason, o.cancel_reason});
    if (hooks_ != nullptr) hooks_->on_order_update(o);
}

void WorkingOrderModel::fill(Order& o, Price price, Qty qty, Timestamp ts, Touch touch) {
    FINFORGE_CHECK(qty > Qty{0} && qty <= o.remaining, "fill quantity out of range");
    FINFORGE_CHECK(price > Price{0}, "fill price must be positive");
    // Mode A fills have no resting order: resting id and owner are 0. All are taker (CND-7, QTE-4).
    const Trade t{TradeId{next_trade_++},
                  o.instrument,
                  price,
                  qty,
                  o.id,
                  OrderId{0},
                  o.owner,
                  OwnerId{0},
                  o.side,
                  ts,
                  touch.bid,
                  touch.ask};
    events_.push_back(t);
    if (hooks_ != nullptr) hooks_->on_fill(o, t, Liquidity::Taker);
    o.filled += qty;
    o.remaining -= qty;
    set_status(o, o.remaining == Qty{0} ? OrderStatus::Filled : OrderStatus::PartiallyFilled);
}

void WorkingOrderModel::cancel_remaining(Order& o, CancelReason reason) {
    o.cancelled += o.remaining;
    o.remaining = Qty{0};
    o.cancel_reason = reason;
    set_status(o, OrderStatus::Cancelled);
}

Qty WorkingOrderModel::affordable(const Order& o, Price price, Qty wanted) {
    if (hooks_ == nullptr || o.side != Side::Buy) return wanted;
    return std::min(hooks_->affordable(o, price, wanted), wanted);
}

Qty WorkingOrderModel::execute(Order& o, Price price, Qty available, Timestamp ts, Touch touch) {
    const bool fok = o.type == OrderType::Limit && o.tif == Tif::Fok;
    const Qty want = std::min(available, o.remaining);
    if (fok && want < o.remaining) {  // CND-4, QTE-2: all or nothing, on the first chance
        cancel_remaining(o, CancelReason::FokUnfillable);
        return Qty{0};
    }
    if (want > Qty{0}) {
        const Qty can = affordable(o, price, want);
        if (can < want) {  // OP-1
            if (fok) {
                cancel_remaining(o, CancelReason::InsufficientCashAtFill);
                return Qty{0};
            }
            if (can > Qty{0}) fill(o, price, can, ts, touch);
            cancel_remaining(o, CancelReason::InsufficientCashAtFill);
            return can;
        }
        fill(o, price, want, ts, touch);
    }
    if (!is_terminal(o.status)) {
        if (o.type == OrderType::Market) {
            cancel_remaining(o, CancelReason::MarketRemainder);  // ORD-6
        } else if (o.tif == Tif::Ioc) {
            cancel_remaining(o, CancelReason::IocRemainder);
        }
    }
    return want;
}

// --- Slippage -----------------------------------------------------------------------------------

Price Slippage::apply(Price price, Side side) const {
    if (kind == SlippageKind::None || rate == Rate{0}) return price;
    const int128 den = kind == SlippageKind::FixedBps ? 1'000'000 : 2'000'000;
    const int128 p = price.value();
    int128 r;
    if (side == Side::Buy) {
        const int128 num = p * (den + rate.value());
        r = (num + den - 1) / den;  // NUM-5: buys round up (num > 0)
    } else {
        r = p * (den - rate.value()) / den;  // sells round down (truncation, num >= 0)
        if (r < 1) r = 1;
    }
    if (r > std::numeric_limits<std::int64_t>::max())
        throw NumericOverflow("NUMERIC_OVERFLOW: slippage");
    return Price{static_cast<std::int64_t>(r)};
}

// --- CandleModel --------------------------------------------------------------------------------

CandleModel::CandleModel(InstrumentRegistry registry, CandleConfig config)
    : WorkingOrderModel(std::move(registry)), config_(config) {
    // At most 100 % of the bar's volume: also keeps participation × volume within int64 (NUM-4).
    FINFORGE_CHECK(config_.participation >= Rate{0} && config_.participation <= Rate{1'000'000},
                   "participation must be in [0, 1]");
    FINFORGE_CHECK(config_.slippage.rate >= Rate{0} && config_.slippage.rate < Rate{1'000'000},
                   "slippage rate must be in [0, 1)");
}

Price CandleModel::market_reference(const NewOrder& o) const {
    const auto m = mark(o.instrument);
    return m ? config_.slippage.apply(*m, o.side) : Price{0};
}

void CandleModel::on_market_data(const MarketData& md) {
    events_.clear();
    const Bar* bar = std::get_if<Bar>(&md);
    if (bar == nullptr || bar->instrument.value() >= marks_.size()) return;
    marks_[bar->instrument.value()] = bar->close;  // ACC-6

    // CND-5: one volume budget per owner for this bar, used in seq order. participation <= 1, so
    // the cap is <= the bar's volume and the narrowing below cannot overflow.
    const bool capped = config_.participation > Rate{0};
    const Qty cap{static_cast<std::int64_t>(static_cast<int128>(config_.participation.value()) *
                                            bar->volume.value() / 1'000'000)};
    std::map<OwnerId, Qty> used;

    const std::vector<OrderId> ids = working(bar->instrument);  // seq order
    for (const OrderId id : ids) {
        Order& o = at(id);
        // CND-1: eligible on the first bar that opens at or after the order was sent. Sent at bar
        // t's close (= bar t+1's open) -> bar t+1; sent mid-bar -> the bar after next.
        if (bar->open_time < o.sent_ts) continue;

        bool crosses = true;
        Price price = config_.slippage.apply(bar->open, o.side);  // CND-2, SLP-1
        if (o.type == OrderType::Limit) {                         // CND-3, strict trade-through
            if (o.side == Side::Buy) {
                crosses = bar->low < o.price;
                price = std::min(bar->open, o.price);
            } else {
                crosses = bar->high > o.price;
                price = std::max(bar->open, o.price);
            }
        }
        Qty available = crosses ? o.remaining : Qty{0};
        if (capped) available = std::min(available, cap - used[o.owner]);
        used[o.owner] += execute(o, price, available, bar->close_time, {bar->open, bar->open});
    }
    compact(bar->instrument);
}

// --- QuoteModel ---------------------------------------------------------------------------------

QuoteModel::QuoteModel(InstrumentRegistry registry) : WorkingOrderModel(std::move(registry)) {}

Price QuoteModel::market_reference(const NewOrder& o) const {
    return mark(o.instrument).value_or(Price{0});  // no slippage in the Quote model (SLP-2)
}

void QuoteModel::on_market_data(const MarketData& md) {
    events_.clear();
    const Quote* q = std::get_if<Quote>(&md);
    if (q == nullptr || q->instrument.value() >= marks_.size()) return;
    const bool has_bid = q->bid > Price{0} && q->bid_qty > Qty{0};
    const bool has_ask = q->ask > Price{0} && q->ask_qty > Qty{0};
    if (q->bid > Price{0} && q->ask > Price{0}) {
        marks_[q->instrument.value()] = Price{(q->bid.value() + q->ask.value()) / 2};  // ACC-6
    }

    // QTE-3: each owner consumes this quote's displayed sizes in seq order, once.
    std::map<OwnerId, std::pair<Qty, Qty>> used;  // (ask lots, bid lots)
    const std::vector<OrderId> ids = working(q->instrument);
    for (const OrderId id : ids) {
        Order& o = at(id);
        // QTE-1: the first quote at or after arrival. Working orders arrived at or before now.
        FINFORGE_CHECK(o.submit_ts <= q->ts, "working order from the future");
        // QTE-1: the order's chance is the first quote that *has* the side it trades against. A
        // one-sided quote is skipped, so MARKET / IOC / FOK orders are not cancelled by it.
        if (!(o.side == Side::Buy ? has_ask : has_bid)) continue;
        auto& [ask_used, bid_used] = used[o.owner];
        Qty available{0};
        Price price;
        if (o.side == Side::Buy) {
            price = q->ask;
            if (o.type == OrderType::Market || q->ask <= o.price) {  // QTE-2
                available = q->ask_qty - ask_used;
            }
            ask_used += execute(o, price, available, q->ts, {q->bid, q->ask});
        } else {
            price = q->bid;
            if (o.type == OrderType::Market || q->bid >= o.price) {
                available = q->bid_qty - bid_used;
            }
            bid_used += execute(o, price, available, q->ts, {q->bid, q->ask});
        }
    }
    compact(q->instrument);
}

}  // namespace finforge
