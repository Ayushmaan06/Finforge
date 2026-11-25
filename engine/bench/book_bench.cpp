// Order book benchmarks (PLAN Phase 1 task 10). Every workload is a pre-generated operation
// stream with a fixed seed, so runs are comparable; generation is outside the timed region.
// Each iteration replays the whole stream into a fresh engine and reports operations per second.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <random>
#include <vector>

#include "finforge/matching_engine.hpp"

using namespace finforge;

namespace {

constexpr std::size_t kOps = 100'000;
constexpr std::int64_t kMid = 100'000;
constexpr InstrumentId kInst{0};

struct Op {
    bool is_cancel = false;
    OrderId cancel_id;
    NewOrder order;
};

InstrumentRegistry registry() {
    InstrumentRegistry r;
    r.add({.symbol = "BENCH", .quote_currency = "USD", .tick_size = "1", .lot_size = "1"});
    return r;
}

class Gen {
  public:
    explicit Gen(std::uint64_t seed) : rng_(seed) {}
    std::int64_t range(std::int64_t lo, std::int64_t hi) {
        return lo + static_cast<std::int64_t>(rng_() % static_cast<std::uint64_t>(hi - lo + 1));
    }
    bool chance(int pct) { return range(1, 100) <= pct; }

  private:
    std::mt19937_64 rng_;
};

// Builds a stream. Passive orders never cross (buys below mid, sells above), so the generator
// knows exactly which ids are still open and can cancel them; ids are dense from 1.
class StreamBuilder {
  public:
    explicit StreamBuilder(std::uint64_t seed) : g_(seed) {}

    void passive(int levels) {
        NewOrder o;
        o.instrument = kInst;
        o.side = g_.chance(50) ? Side::Buy : Side::Sell;
        const std::int64_t off = g_.range(1, levels);
        o.price = Price{o.side == Side::Buy ? kMid - off : kMid + off};
        o.qty = Qty{g_.range(1, 10)};
        push(o);
        open_.push_back(OrderId{next_id_ - 1});
    }

    void aggressive(int levels) {
        NewOrder o;
        o.instrument = kInst;
        o.side = g_.chance(50) ? Side::Buy : Side::Sell;
        o.tif = Tif::Ioc;  // never rests, so `open_` stays exact
        const std::int64_t off = g_.range(1, levels);
        o.price = Price{o.side == Side::Buy ? kMid + off : kMid - off};
        o.qty = Qty{g_.range(5, 30)};
        push(o);
    }

    // Cancels a random still-open passive order (it may have been filled meanwhile; then the
    // engine answers ALREADY_TERMINAL, which is still a realistic lookup).
    void cancel() {
        if (open_.empty()) return passive(50);
        const auto i = static_cast<std::size_t>(g_.range(0, open_.size() - 1));
        ops_.push_back(Op{true, open_[i], {}});
        open_[i] = open_.back();
        open_.pop_back();
    }

    Gen& gen() { return g_; }
    std::vector<Op> take() { return std::move(ops_); }

  private:
    void push(const NewOrder& o) {
        ops_.push_back(Op{false, {}, o});
        ++next_id_;
    }

    Gen g_;
    std::vector<Op> ops_;
    std::vector<OrderId> open_;
    std::uint64_t next_id_ = 1;
};

std::vector<Op> add_heavy() {
    StreamBuilder b(1);
    for (std::size_t i = 0; i < kOps; ++i) b.gen().chance(90) ? b.passive(50) : b.cancel();
    return b.take();
}

std::vector<Op> cancel_heavy() {
    StreamBuilder s(2);
    for (std::size_t i = 0; i < 2'000; ++i) s.passive(50);  // seed the book
    for (std::size_t i = 2'000; i < kOps; ++i) s.gen().chance(40) ? s.passive(50) : s.cancel();
    return s.take();
}

std::vector<Op> match_heavy() {
    StreamBuilder b(3);
    for (std::size_t i = 0; i < kOps; ++i) b.gen().chance(55) ? b.passive(5) : b.aggressive(3);
    return b.take();
}

std::vector<Op> mixed() {
    StreamBuilder b(4);
    for (std::size_t i = 0; i < kOps; ++i) {
        const auto r = b.gen().range(1, 100);
        if (r <= 50) {
            b.passive(20);
        } else if (r <= 80) {
            b.cancel();
        } else {
            b.aggressive(5);
        }
    }
    return b.take();
}

void run(benchmark::State& state, const std::vector<Op>& ops) {
    std::uint64_t trades = 0;
    for (auto _ : state) {
        MatchingEngine eng(registry());
        for (const Op& op : ops) {
            if (op.is_cancel) {
                benchmark::DoNotOptimize(eng.cancel(op.cancel_id));
            } else {
                benchmark::DoNotOptimize(eng.submit(op.order));
            }
            for (const auto& e : eng.events()) trades += std::holds_alternative<Trade>(e);
        }
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * ops.size()));
    state.counters["trades/op"] =
        static_cast<double>(trades) / static_cast<double>(state.iterations() * ops.size());
}

void BM_AddHeavy(benchmark::State& s) {
    static const auto ops = add_heavy();
    run(s, ops);
}
void BM_CancelHeavy(benchmark::State& s) {
    static const auto ops = cancel_heavy();
    run(s, ops);
}
void BM_MatchHeavy(benchmark::State& s) {
    static const auto ops = match_heavy();
    run(s, ops);
}
void BM_Mixed(benchmark::State& s) {
    static const auto ops = mixed();
    run(s, ops);
}

}  // namespace

BENCHMARK(BM_AddHeavy)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_CancelHeavy)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_MatchHeavy)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Mixed)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
