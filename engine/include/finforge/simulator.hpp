#pragma once

// Discrete-event simulator (contracts.md §6). The clock jumps from event to event in EventKey
// order; handlers may only schedule events with a strictly greater key (EVT-4), which holds by
// construction because every action is delayed by latency >= 1 ns (EVT-3) and timers must be in the
// future.

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

#include "finforge/event_queue.hpp"
#include "finforge/events.hpp"
#include "finforge/execution_model.hpp"
#include "finforge/journal.hpp"
#include "finforge/strategy.hpp"

namespace finforge {

struct SimProgress {
    std::uint64_t events = 0;  // processed so far
    Timestamp now;
    std::size_t queued = 0;
};

struct SimConfig {
    Timestamp start;
    std::int64_t latency_ns = 1;            // EVT-3: >= 1
    std::int64_t snapshot_interval_ns = 0;  // 0 = no snapshots
    // Called after every `progress_every` processed events (0 = never).
    std::uint64_t progress_every = 0;
    std::function<void(const SimProgress&)> on_progress;
    // Checked before an event whenever events_processed % cancel_check_every == 0. Setting it stops
    // the run between two events, so all state is consistent.
    const std::atomic<bool>* cancel_flag = nullptr;
    std::uint64_t cancel_check_every = 1024;
};

enum class RunStatus : std::uint8_t {
    Completed,          // queue empty
    ReachedTime,        // run_until: no events left at or before the limit
    ReachedEventLimit,  // max_events processed in this call
    Cancelled,          // cancel flag seen
};

class Simulator {
  public:
    static constexpr std::uint64_t kUnlimited = std::numeric_limits<std::uint64_t>::max();

    // `model` and `journal` must outlive the simulator. Throws std::invalid_argument on bad config.
    Simulator(ExecutionModel& model, Journal& journal, SimConfig config = {});
    ~Simulator();
    Simulator(const Simulator&) = delete;
    Simulator& operator=(const Simulator&) = delete;

    // Registers a strategy before the run starts; owner ids are dense from 1. Not owned.
    OwnerId add_strategy(Strategy& strategy);

    // Queues external market data at its emit time (TIME-2, TIME-4). Throws std::invalid_argument
    // if that time is not after the current clock once the run has started.
    void schedule_market_data(const MarketData& md);

    RunStatus run(std::uint64_t max_events = kUnlimited);
    // Processes every event with ts <= limit, then advances the clock to `limit`.
    RunStatus run_until(Timestamp limit, std::uint64_t max_events = kUnlimited);
    // Processes one event. False if the queue is empty.
    bool step();

    [[nodiscard]] Timestamp now() const { return now_; }
    [[nodiscard]] std::uint64_t events_processed() const { return processed_; }
    [[nodiscard]] std::size_t queued() const { return queue_.size(); }
    [[nodiscard]] const EventKey& last_key() const { return last_key_; }
    [[nodiscard]] const ExecutionModel& model() const { return model_; }

  private:
    class StrategyContext;

    void start_if_needed();
    bool cancelled() const;
    void process(QueuedEvent ev);
    void deliver(std::span<const BookEvent> events);
    Strategy* strategy_of(OwnerId owner);
    StrategyContext& context_of(OwnerId owner);
    void push(Timestamp ts, InstrumentId instrument, Event event);
    void schedule_snapshot(Timestamp ts);

    ExecutionModel& model_;
    Journal& journal_;
    SimConfig config_;
    EventQueue queue_;
    std::vector<Strategy*> strategies_;  // index = owner - 1
    std::vector<std::unique_ptr<StrategyContext>> contexts_;
    std::vector<std::uint64_t> client_refs_;  // index = OrderId - 1
    std::uint64_t next_client_ref_ = 1;
    Timestamp now_;
    EventKey last_key_{};
    EventKey current_key_{};
    std::uint64_t processed_ = 0;
    bool started_ = false;
    bool processing_ = false;  // inside process(): EVT-4 applies to anything scheduled
};

}  // namespace finforge
