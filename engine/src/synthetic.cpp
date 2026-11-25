#include "finforge/synthetic.hpp"

#include <algorithm>
#include <memory>

namespace finforge {

void SyntheticAgent::schedule_next(Context& ctx) {
    // Poisson arrivals: exponential gaps; at least 1 ns so the timer is strictly in the future.
    const std::int64_t gap = std::max<std::int64_t>(1, rng_.exponential(cfg_.mean_interarrival_ns));
    ctx.set_timer(Timestamp{checked_add(ctx.now().value(), gap)}, 0);
}

Price SyntheticAgent::mid(const Context& ctx) const {
    const OrderBook* b = ctx.book(cfg_.instrument);
    if (b != nullptr) {
        const auto bid = b->best_bid();
        const auto ask = b->best_ask();
        if (bid && ask) return Price{(bid->value() + ask->value()) / 2};
        if (bid) return *bid;
        if (ask) return *ask;
    }
    return cfg_.reference_mid;
}

void SyntheticAgent::on_timer(Context& ctx, std::uint64_t) {
    const std::uint32_t r = rng_.below(100);
    if (!open_.empty() && (r < cfg_.pct_cancel || open_.size() >= cfg_.max_open)) {
        // The order may fill before the cancel arrives; then it is refused ALREADY_TERMINAL.
        ctx.cancel(open_[rng_.below(static_cast<std::uint32_t>(open_.size()))]);
    } else {
        OrderRequest req;
        req.instrument = cfg_.instrument;
        req.side = rng_.chance(50) ? Side::Buy : Side::Sell;
        req.qty = Qty{rng_.range(1, cfg_.max_qty)};
        if (r < cfg_.pct_cancel + cfg_.pct_market) {
            req.type = OrderType::Market;
        } else {
            req.type = OrderType::Limit;
            req.tif = rng_.chance(cfg_.pct_ioc) ? Tif::Ioc : Tif::Gtc;
            // Mostly passive (away from the mid on the order's own side); a quarter of the range
            // crosses the mid and is likely to trade.
            const std::int64_t off = rng_.range(-cfg_.max_offset_ticks / 4, cfg_.max_offset_ticks);
            const std::int64_t m = mid(ctx).value();
            const std::int64_t px = req.side == Side::Buy ? m - off : m + off;
            req.price = Price{std::max<std::int64_t>(1, px)};
        }
        ctx.submit(req);
    }
    schedule_next(ctx);
}

void SyntheticAgent::on_order_update(Context&, const OwnOrderUpdate& u) {
    const OrderId id = u.update.order;
    if (u.update.status == OrderStatus::Accepted) {
        open_.push_back(id);
    } else if (is_terminal(u.update.status)) {
        std::erase(open_, id);
    }
}

InstrumentRegistry SyntheticMarket::registry() {
    InstrumentRegistry reg;
    reg.add({.symbol = "SIM", .quote_currency = "USD", .tick_size = "1", .lot_size = "1"});
    return reg;
}

SyntheticMarket::SyntheticMarket(const SyntheticRunConfig& cfg)
    : config_(cfg), model_(registry()), journal_(cfg.journal_sink) {
    SimConfig sc;
    sc.latency_ns = cfg.latency_ns;
    sc.snapshot_interval_ns = cfg.snapshot_interval_ns;
    sc.cancel_flag = cfg.cancel_flag;
    sc.cancel_check_every = cfg.cancel_check_every;
    sc.progress_every = cfg.progress_every;
    sc.on_progress = cfg.on_progress;
    sim_ = std::make_unique<Simulator>(model_, journal_, sc);

    SplitMix64 seeds(cfg.seed);
    for (std::size_t i = 0; i < cfg.agents; ++i) {
        AgentConfig ac;
        ac.instrument = instrument();
        // Agents differ in speed: mean gaps of 0.5 ms, 1 ms, 1.5 ms, ...
        ac.mean_interarrival_ns = 500'000 * static_cast<std::int64_t>(i + 1);
        const std::uint64_t state = seeds.next();
        const std::uint64_t stream = seeds.next();
        agents_.push_back(std::make_unique<SyntheticAgent>(ac, state, stream));
        sim_->add_strategy(*agents_.back());
    }
}

RunStatus SyntheticMarket::run(std::uint64_t max_events) {
    const std::uint64_t every = config_.invariant_check_every;
    if (every == 0) return sim_->run(max_events);
    RunStatus status = RunStatus::ReachedEventLimit;
    while (max_events > 0 && status == RunStatus::ReachedEventLimit) {
        const std::uint64_t n = std::min(every, max_events);
        status = sim_->run(n);
        model_.engine().check_invariants();
        max_events -= n;
    }
    return status;
}

SyntheticRunResult SyntheticMarket::result(RunStatus status) const {
    const MatchingEngine& eng = model_.engine();
    SyntheticRunResult r;
    r.status = status;
    r.events = sim_->events_processed();
    r.orders = eng.order_count();
    r.trades = eng.trade_count();
    r.journal_records = journal_.records();
    r.journal_bytes = journal_.bytes();
    r.journal_sha256 = journal_.hash_hex();
    r.end_time = sim_->now();
    r.best_bid = eng.book(instrument()).best_bid();
    r.best_ask = eng.book(instrument()).best_ask();
    r.open_orders = eng.open_order_count();
    return r;
}

SyntheticRunResult run_synthetic(const SyntheticRunConfig& cfg) {
    SyntheticMarket market(cfg);
    const RunStatus status = market.run();
    return market.result(status);
}

}  // namespace finforge
