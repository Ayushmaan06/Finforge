#include "finforge/simulator.hpp"

#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>

#include "finforge/assert.hpp"

namespace finforge {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

class Simulator::StrategyContext final : public Context {
  public:
    StrategyContext(Simulator& sim, OwnerId owner) : sim_(sim), owner_(owner) {}

    Timestamp now() const override { return sim_.now_; }
    OwnerId owner() const override { return owner_; }

    std::uint64_t submit(const OrderRequest& r) override {
        const std::uint64_t ref = sim_.next_client_ref_++;
        NewOrder o{
            owner_,  r.instrument, r.side,      r.type,   r.tif,
            r.price, r.qty,        Timestamp{}, sim_.now_};  // sent now; arrives at now + latency
        sim_.push(arrival(), r.instrument, SubmitAction{owner_, ref, o});
        return ref;
    }

    void cancel(OrderId id) override {
        sim_.push(arrival(), instrument_of_order(id), CancelAction{owner_, id});
    }

    void amend(OrderId id, Qty q, Price p) override {
        sim_.push(arrival(), instrument_of_order(id), AmendAction{owner_, id, q, p});
    }

    void set_timer(Timestamp at, std::uint64_t tag) override {
        if (at <= sim_.now_) throw std::invalid_argument("set_timer: time must be after now()");
        sim_.push(at, InstrumentId{}, TimerEvent{owner_, tag});
    }

    const OrderBook* book(InstrumentId id) const override { return sim_.model_.book(id); }

    const Ledger* account() const override {
        return sim_.accounts_ != nullptr ? sim_.accounts_->ledger(owner_) : nullptr;
    }

  private:
    // EVT-3: everything a strategy does reaches the exchange `latency` later.
    Timestamp arrival() const {
        return Timestamp{checked_add(sim_.now_.value(), sim_.config_.latency_ns)};
    }
    InstrumentId instrument_of_order(OrderId id) const {
        const Order* o = sim_.model_.order(id);
        return o != nullptr ? o->instrument : InstrumentId{};
    }

    Simulator& sim_;
    OwnerId owner_;
};

Simulator::Simulator(ExecutionModel& model, Journal& journal, SimConfig config)
    : model_(model), journal_(journal), config_(std::move(config)), now_(config_.start) {
    if (config_.latency_ns < 1) throw std::invalid_argument("latency_ns must be >= 1 (EVT-3)");
    if (config_.snapshot_interval_ns < 0) throw std::invalid_argument("snapshot_interval_ns < 0");
    if (config_.cancel_check_every < 1) throw std::invalid_argument("cancel_check_every < 1");
}

Simulator::~Simulator() = default;

OwnerId Simulator::add_strategy(Strategy& strategy) {
    if (started_) throw std::logic_error("add_strategy after the run started");
    strategies_.push_back(&strategy);
    const OwnerId owner{static_cast<OwnerId::rep>(strategies_.size())};
    contexts_.push_back(std::make_unique<StrategyContext>(*this, owner));
    return owner;
}

void Simulator::schedule_market_data(const MarketData& md) {
    const Timestamp t = emit_time(md);
    if (started_ && t <= now_) {
        throw std::invalid_argument("market data must be scheduled after the current time");
    }
    std::visit([&](const auto& m) { push(t, instrument_of(md), Event{m}); }, md);
}

void Simulator::attach_source(MarketDataSource& source) {
    if (started_) throw std::logic_error("attach_source after the run started");
    if (source_ != nullptr) throw std::logic_error("a market data source is already attached");
    source_ = &source;
}

void Simulator::attach_accounts(Accounts& accounts) {
    if (started_) throw std::logic_error("attach_accounts after the run started");
    if (accounts_ != nullptr) throw std::logic_error("accounts are already attached");
    if (model_.account_hooks() != &accounts) {
        throw std::invalid_argument("accounts belong to a different execution model");
    }
    accounts_ = &accounts;
}

void Simulator::finish() {
    if (finished_) throw std::logic_error("finish() called twice");
    start_if_needed();
    finished_ = true;
    model_.cancel_all(CancelReason::EndOfRun);  // OP-3
    deliver(model_.events());
    if (accounts_ != nullptr) accounts_->finish(now_);
}

void Simulator::pull_source() {
    MarketData md;
    if (!source_->next(md)) return;
    const Timestamp t = emit_time(md);
    // Keys must keep increasing (EVT-4): the source may not go back in time, nor back to a lower
    // instrument id at the same time.
    const EventKey probe{t, EventClass::MarketData, instrument_of(md),
                         Seq{std::numeric_limits<Seq::rep>::max()}};
    if (t < now_ || (processing_ && probe < current_key_)) {
        throw std::invalid_argument("market data source is not in emit order");
    }
    source_key_ =
        std::visit([&](const auto& m) { return queue_.push(t, instrument_of(md), Event{m}); }, md);
    source_queued_ = true;
}

void Simulator::push(Timestamp ts, InstrumentId instrument, Event event) {
    if (finished_) return;  // the run is over
    const EventKey key = queue_.push(ts, instrument, std::move(event));
    // EVT-4: an event handler may only schedule events after itself.
    FINFORGE_CHECK(!processing_ || key > current_key_,
                   "scheduled an event that is not after the current one (EVT-4)");
}

void Simulator::schedule_snapshot(Timestamp ts) { push(ts, InstrumentId{}, SnapshotEvent{}); }

void Simulator::start_if_needed() {
    if (started_) return;
    started_ = true;
    if (config_.snapshot_interval_ns > 0) {
        schedule_snapshot(Timestamp{config_.start.value() + config_.snapshot_interval_ns});
    }
    for (std::size_t i = 0; i < strategies_.size(); ++i) strategies_[i]->on_start(*contexts_[i]);
    if (source_ != nullptr) pull_source();
}

bool Simulator::cancelled() const {
    return config_.cancel_flag != nullptr && processed_ % config_.cancel_check_every == 0 &&
           config_.cancel_flag->load(std::memory_order_relaxed);
}

RunStatus Simulator::run(std::uint64_t max_events) {
    start_if_needed();
    for (std::uint64_t n = 0; n < max_events; ++n) {
        if (queue_.empty()) return RunStatus::Completed;
        if (cancelled()) return RunStatus::Cancelled;
        process(queue_.pop());
    }
    return queue_.empty() ? RunStatus::Completed : RunStatus::ReachedEventLimit;
}

RunStatus Simulator::run_until(Timestamp limit, std::uint64_t max_events) {
    start_if_needed();
    for (std::uint64_t n = 0; n < max_events; ++n) {
        if (queue_.empty() || queue_.top().key.ts > limit) {
            if (limit > now_) now_ = limit;
            return queue_.empty() ? RunStatus::Completed : RunStatus::ReachedTime;
        }
        if (cancelled()) return RunStatus::Cancelled;
        process(queue_.pop());
    }
    return RunStatus::ReachedEventLimit;
}

bool Simulator::step() {
    start_if_needed();
    if (queue_.empty()) return false;
    process(queue_.pop());
    return true;
}

Strategy* Simulator::strategy_of(OwnerId owner) {
    const auto i = owner.value();
    return (i >= 1 && i <= strategies_.size()) ? strategies_[i - 1] : nullptr;
}

Simulator::StrategyContext& Simulator::context_of(OwnerId owner) {
    return *contexts_[owner.value() - 1];
}

void Simulator::process(QueuedEvent ev) {
    // EVT-4 / TIME-5: keys strictly increase, so the clock never goes back.
    FINFORGE_CHECK(processed_ == 0 || ev.key > last_key_, "event keys must strictly increase");
    FINFORGE_CHECK(ev.key.ts >= now_, "clock would go backwards (TIME-5)");
    current_key_ = ev.key;
    now_ = ev.key.ts;
    processing_ = true;
    if (accounts_ != nullptr) accounts_->advance(now_);  // ACC-8 borrow, interval snapshots
    // Queue the next source event before handling this one, so a snapshot at this time still sees
    // a non-empty queue and keeps the run going.
    if (source_queued_ && ev.key == source_key_) {
        source_queued_ = false;
        pull_source();
    }

    if (std::holds_alternative<SnapshotEvent>(ev.event)) {
        std::vector<const OrderBook*> books;
        for (std::size_t i = 0; i < model_.instrument_count(); ++i) {
            if (const OrderBook* b = model_.book(InstrumentId{static_cast<std::uint32_t>(i)})) {
                books.push_back(b);
            }
        }
        journal_.record_snapshot(ev.key, books);
        // Only while something else is pending; otherwise snapshots alone would never end the run.
        if (!queue_.empty()) {
            schedule_snapshot(Timestamp{now_.value() + config_.snapshot_interval_ns});
        }
    } else {
        journal_.record_input(ev.key, ev.event);
    }

    // Refuses a cancel/amend for an order that is unknown or belongs to someone else.
    auto own_order = [&](OwnerId owner, OrderId id) {
        const Order* o = model_.order(id);
        return o != nullptr && o->owner == owner;
    };
    auto refuse = [&](OwnerId owner, OrderId id, RejectReason reason) {
        journal_.record_refused(ev.key, owner, id, reason);
        if (Strategy* s = strategy_of(owner)) s->on_action_refused(context_of(owner), id, reason);
    };

    std::visit(Overloaded{
                   [&](SubmitAction& a) {
                       a.order.owner = a.owner;
                       a.order.ts = now_;
                       const OrderId id = model_.submit(a.order);
                       if (client_refs_.size() < id.value()) client_refs_.resize(id.value());
                       client_refs_[id.value() - 1] = a.client_ref;
                       deliver(model_.events());
                   },
                   [&](CancelAction& a) {
                       if (!own_order(a.owner, a.order)) {
                           return refuse(a.owner, a.order, RejectReason::UnknownOrder);
                       }
                       const ActionResult r = model_.cancel(a.order);
                       if (!r.ok()) return refuse(a.owner, a.order, r.reason);
                       deliver(model_.events());
                   },
                   [&](AmendAction& a) {
                       if (!own_order(a.owner, a.order)) {
                           return refuse(a.owner, a.order, RejectReason::UnknownOrder);
                       }
                       const Timestamp sent{now_.value() - config_.latency_ns};
                       const ActionResult r = model_.amend(a.order, a.qty, a.price, now_, sent);
                       if (!r.ok()) return refuse(a.owner, a.order, r.reason);
                       if (r.new_order.value() != 0) {
                           // The replacement keeps the strategy's client reference.
                           if (client_refs_.size() < r.new_order.value()) {
                               client_refs_.resize(r.new_order.value());
                           }
                           client_refs_[r.new_order.value() - 1] =
                               client_refs_[a.order.value() - 1];
                       }
                       deliver(model_.events());
                   },
                   [&](TimerEvent& t) {
                       if (Strategy* s = strategy_of(t.owner))
                           s->on_timer(context_of(t.owner), t.tag);
                   },
                   [&](SnapshotEvent&) {},
                   [&](auto& md) {  // Bar, Quote, TradePrint
                       const MarketData data{md};
                       model_.on_market_data(data);
                       deliver(model_.events());
                       if (accounts_ != nullptr) accounts_->after_market_data(data, now_);
                       for (std::size_t i = 0; i < strategies_.size(); ++i) {
                           strategies_[i]->on_market_data(*contexts_[i], data);
                       }
                   },
               },
               ev.event);

    processing_ = false;
    last_key_ = ev.key;
    ++processed_;
    if (config_.progress_every > 0 && config_.on_progress &&
        processed_ % config_.progress_every == 0) {
        config_.on_progress(SimProgress{processed_, now_, queue_.size()});
    }
}

void Simulator::deliver(std::span<const BookEvent> events) {
    // Strategies only schedule events from these callbacks; they never call the model directly,
    // so `events` stays valid for the whole loop.
    for (const BookEvent& e : events) {
        journal_.record_output(e);
        std::visit(Overloaded{
                       [&](const OrderUpdate& u) {
                           const OwnerId owner = model_.order(u.order)->owner;
                           if (Strategy* s = strategy_of(owner)) {
                               s->on_order_update(context_of(owner),
                                                  {client_refs_[u.order.value() - 1], u});
                           }
                       },
                       [&](const OrderAmended& a) {
                           const OwnerId owner = model_.order(a.order)->owner;
                           if (Strategy* s = strategy_of(owner)) {
                               s->on_order_amended(context_of(owner), a);
                           }
                       },
                       [&](const Trade& t) {
                           if (Strategy* s = strategy_of(t.resting_owner)) {
                               s->on_fill(context_of(t.resting_owner), t);
                           }
                           if (Strategy* s = strategy_of(t.aggressor_owner)) {
                               s->on_fill(context_of(t.aggressor_owner), t);
                           }
                       },
                   },
                   e);
    }
}

}  // namespace finforge
