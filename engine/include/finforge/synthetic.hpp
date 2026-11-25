#pragma once

// Synthetic order flow for Mode B (PLAN Phase 2 task 8). Each agent acts at Poisson arrival times
// and submits limit orders around the current mid, market orders, or cancels of its own orders.
// Each agent has its own PCG32 stream derived from the run seed with splitmix64 (DET-2).

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "finforge/execution_model.hpp"
#include "finforge/journal.hpp"
#include "finforge/rng.hpp"
#include "finforge/simulator.hpp"
#include "finforge/strategy.hpp"

namespace finforge {

struct AgentConfig {
    InstrumentId instrument;
    std::int64_t mean_interarrival_ns = 1'000'000;  // 1 ms
    Price reference_mid{10'000};                    // used while one side of the book is empty
    std::int64_t max_offset_ticks = 20;             // limit prices: mid ± up to this many ticks
    std::int64_t max_qty = 10;
    std::uint32_t pct_cancel = 30;
    std::uint32_t pct_market = 5;
    std::uint32_t pct_ioc = 10;  // of limit orders
    std::size_t max_open = 20;   // above this, the agent cancels instead of adding
};

class SyntheticAgent final : public Strategy {
  public:
    SyntheticAgent(AgentConfig config, std::uint64_t seed_state, std::uint64_t seed_stream)
        : cfg_(config), rng_(seed_state, seed_stream) {}

    void on_start(Context& ctx) override { schedule_next(ctx); }
    void on_timer(Context& ctx, std::uint64_t) override;
    void on_order_update(Context& ctx, const OwnOrderUpdate& u) override;

    [[nodiscard]] std::size_t open_orders() const { return open_.size(); }

  private:
    void schedule_next(Context& ctx);
    Price mid(const Context& ctx) const;

    AgentConfig cfg_;
    Pcg32 rng_;
    std::vector<OrderId> open_;  // insertion order, so choices are deterministic
};

struct SyntheticRunConfig {
    std::uint64_t seed = 42;
    std::uint64_t max_events = 1'000'000;
    std::size_t agents = 8;
    std::int64_t latency_ns = 1'000;                    // 1 µs
    std::int64_t snapshot_interval_ns = 1'000'000'000;  // 1 s
    std::ostream* journal_sink = nullptr;
    std::uint64_t progress_every = 0;
    std::function<void(const SimProgress&)> on_progress;
    const std::atomic<bool>* cancel_flag = nullptr;
    std::uint64_t cancel_check_every = 1024;
    // Runs MatchingEngine::check_invariants() every N events (0 = never).
    std::uint64_t invariant_check_every = 0;
};

struct SyntheticRunResult {
    RunStatus status = RunStatus::Completed;
    std::uint64_t events = 0;
    std::uint64_t orders = 0;
    std::uint64_t trades = 0;
    std::uint64_t journal_records = 0;
    std::uint64_t journal_bytes = 0;
    std::string journal_sha256;
    Timestamp end_time;
    std::optional<Price> best_bid;
    std::optional<Price> best_ask;
    std::size_t open_orders = 0;
};

// One instrument ("SIM", tick 1, lot 1) and `agents` SyntheticAgents with different arrival rates,
// wired to a BookModel, a Journal and a Simulator. The same config always gives the same journal.
class SyntheticMarket {
  public:
    explicit SyntheticMarket(const SyntheticRunConfig& config);

    // Runs up to config.max_events more events (or `max_events` if given), checking book
    // invariants every config.invariant_check_every events.
    RunStatus run(std::uint64_t max_events);
    RunStatus run() { return run(config_.max_events); }
    [[nodiscard]] SyntheticRunResult result(RunStatus status) const;

    Simulator& sim() { return *sim_; }
    [[nodiscard]] const BookModel& model() const { return model_; }
    [[nodiscard]] const Journal& journal() const { return journal_; }
    [[nodiscard]] InstrumentId instrument() const { return InstrumentId{0}; }

  private:
    static InstrumentRegistry registry();

    SyntheticRunConfig config_;
    BookModel model_;
    Journal journal_;
    std::vector<std::unique_ptr<SyntheticAgent>> agents_;
    std::unique_ptr<Simulator> sim_;
};

SyntheticRunResult run_synthetic(const SyntheticRunConfig& config);

}  // namespace finforge
