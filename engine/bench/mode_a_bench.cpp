// Mode A engine throughput (PLAN §10: "Engine, C++ no-op strategy, 1M bars < 1 s"). A full run as
// the Python SDK does it, minus Python: streamed bars → simulator → CandleModel → accounts (equity
// snapshot per bar) → journal (SHA-256 of every record). Bars come from a seeded random walk
// generated on the fly, so the measurement excludes Parquet I/O (Phase 3 measured replay alone).

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <random>

#include "finforge/accounts.hpp"
#include "finforge/journal.hpp"
#include "finforge/mode_a.hpp"
#include "finforge/simulator.hpp"
#include "finforge/strategy.hpp"

using namespace finforge;

namespace {

constexpr std::int64_t kBars = 1'000'000;
constexpr std::int64_t kMinute = 60'000'000'000;
constexpr std::int64_t kUnit = 100'000'000;  // Money per currency unit (tick 1, lot 1)
constexpr InstrumentId kInst{0};

InstrumentRegistry registry() {
    InstrumentRegistry r;
    r.add({.symbol = "BENCH", .quote_currency = "USD", .tick_size = "1", .lot_size = "1"});
    return r;
}

// 1-minute bars of a seeded random walk around 100 000 ticks.
class WalkSource final : public MarketDataSource {
  public:
    bool next(MarketData& out) override {
        if (i_ == kBars) return false;
        const std::int64_t open = px_;
        px_ += static_cast<std::int64_t>(rng_() % 21) - 10;
        const std::int64_t close = px_;
        const std::int64_t hi = std::max(open, close) + static_cast<std::int64_t>(rng_() % 5);
        const std::int64_t lo = std::min(open, close) - static_cast<std::int64_t>(rng_() % 5);
        out = Bar{kInst,
                  Timestamp{i_ * kMinute},
                  Timestamp{(i_ + 1) * kMinute},
                  Price{open},
                  Price{hi},
                  Price{lo},
                  Price{close},
                  Qty{1'000}};
        ++i_;
        return true;
    }

  private:
    std::mt19937_64 rng_{7};
    std::int64_t i_ = 0;
    std::int64_t px_ = 100'000;
};

class NoOp final : public Strategy {};

// One market order every 100 bars, alternating buy and sell: exercises the execution model, fills
// and the ledger as well (10 000 orders per run).
class Churn final : public Strategy {
  public:
    void on_market_data(Context& c, const MarketData&) override {
        if (++n_ % 100 != 0) return;
        const Side s = (n_ / 100) % 2 == 1 ? Side::Buy : Side::Sell;
        c.submit(OrderRequest{kInst, s, OrderType::Market, Tif::Gtc, Price{0}, Qty{1}});
    }

  private:
    std::int64_t n_ = 0;
};

template <class S>
void run(benchmark::State& state) {
    for (auto _ : state) {
        CandleModel model(registry());
        Accounts accounts(model);
        Journal journal;
        Simulator sim(model, journal);
        S strategy;
        const OwnerId me = sim.add_strategy(strategy);
        accounts.open(me, AccountConfig{.initial_cash = Money{1'000'000 * kUnit}});
        sim.attach_accounts(accounts);
        WalkSource source;
        sim.attach_source(source);
        benchmark::DoNotOptimize(sim.run());
        sim.finish();
        benchmark::DoNotOptimize(journal.hash_hex());
    }
    state.SetItemsProcessed(state.iterations() * kBars);
}

void BM_ModeA_NoOp_1MBars(benchmark::State& s) { run<NoOp>(s); }
void BM_ModeA_Churn_1MBars(benchmark::State& s) { run<Churn>(s); }

}  // namespace

BENCHMARK(BM_ModeA_NoOp_1MBars)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_ModeA_Churn_1MBars)->Unit(benchmark::kMillisecond);
