// Python bindings: a thin layer over engine/. Logic lives in the engine library.
//
// Market data crosses the boundary zero-copy: Python yields tuples of 1-D C-contiguous numpy arrays
// (int64, plus int8 for trade aggressors); the C++ feed reads their buffers directly and keeps a
// reference to the tuple until it moves on to the next chunk. Arrays of any other dtype or layout
// are rejected with TypeError rather than silently copied.
//
// The GIL is held for the whole run (Phase 3): the chunk iterators are Python code.

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "common.hpp"
#include "finforge/execution_model.hpp"
#include "finforge/instrument.hpp"
#include "finforge/journal.hpp"
#include "finforge/replay.hpp"
#include "finforge/simulator.hpp"
#include "finforge/strategy.hpp"
#include "finforge/version.hpp"

namespace py = pybind11;
using namespace finforge;
using namespace finforge::bindings;

namespace {

// No-op strategy that counts market data and can record what it saw (for merge-order tests).
class CountingStrategy final : public Strategy {
  public:
    void on_market_data(Context& ctx, const MarketData& md) override {
        ++counts[md.index()];
        if (record) {
            ts.push_back(ctx.now().value());
            instrument.push_back(instrument_of(md).value());
            kind.push_back(static_cast<std::uint8_t>(md.index()));
        }
    }
    std::uint64_t counts[3] = {0, 0, 0};
    bool record = false;
    std::vector<std::int64_t> ts;
    std::vector<std::uint32_t> instrument;
    std::vector<std::uint8_t> kind;
};

const char* status_name(RunStatus s) {
    switch (s) {
        case RunStatus::Completed:
            return "completed";
        case RunStatus::ReachedTime:
            return "reached_time";
        case RunStatus::ReachedEventLimit:
            return "reached_event_limit";
        case RunStatus::Cancelled:
            return "cancelled";
    }
    return "?";
}

// A replay run: registry + feeds + a no-op strategy (PLAN Phase 3 task 8).
class Engine {
  public:
    Engine(const py::list& instruments, std::int64_t latency_ns, std::int64_t start_ns) {
        InstrumentRegistry reg = registry_from(instruments);
        n_instruments_ = reg.size();
        model_ = std::make_unique<BookModel>(std::move(reg));
        config_.latency_ns = latency_ns;
        config_.start = Timestamp{start_ns};
    }

    void add_feed(const std::string& kind, std::uint32_t instrument_id,
                  const py::iterable& chunks) {
        if (ran_) throw std::logic_error("add_feed after run()");
        if (instrument_id >= n_instruments_) throw py::value_error("unknown instrument_id");
        providers_.push_back(std::make_unique<PyChunkProvider>(parse_kind(kind), chunks));
        replay_.add_feed(parse_kind(kind), InstrumentId{instrument_id}, *providers_.back());
    }

    std::string run(bool record) {
        if (ran_) throw std::logic_error("an Engine runs once; create a new one");
        ran_ = true;
        strategy_.record = record;
        sim_ = std::make_unique<Simulator>(*model_, journal_, config_);
        sim_->add_strategy(strategy_);
        sim_->attach_source(replay_);
        return status_name(sim_->run());
    }

    [[nodiscard]] py::dict counts() const {
        py::dict d;
        d["bars"] = strategy_.counts[0];
        d["quotes"] = strategy_.counts[1];
        d["trades"] = strategy_.counts[2];
        d["events_processed"] = sim_ ? sim_->events_processed() : 0;
        return d;
    }

    [[nodiscard]] py::dict recorded() const {
        py::dict d;
        d["ts"] = to_numpy(strategy_.ts);
        d["instrument_id"] = to_numpy(strategy_.instrument);
        d["kind"] = to_numpy(strategy_.kind);
        return d;
    }

    [[nodiscard]] std::string journal_hash() const { return journal_.hash_hex(); }

  private:
    std::size_t n_instruments_ = 0;
    std::unique_ptr<BookModel> model_;
    SimConfig config_;
    Journal journal_;
    std::vector<std::unique_ptr<PyChunkProvider>> providers_;
    Replay replay_;
    CountingStrategy strategy_;
    std::unique_ptr<Simulator> sim_;
    bool ran_ = false;
};

}  // namespace

namespace finforge::bindings {
void register_backtest(pybind11::module_& m);
}

PYBIND11_MODULE(_engine, m) {
    m.doc() = "FinForge C++ engine (thin bindings; logic lives in engine/).";
    m.def("engine_version", &finforge::engine_version,
          "Engine version string: <semver>+<git sha>.<build type>.");
    m.def("money_per_tick_lot", &finforge::compute_money_per_tick_lot, py::arg("tick_size"),
          py::arg("lot_size"),
          "NUM-2: tick_size × lot_size / 1e-8 as an exact integer. ValueError if it is not one.");

    py::class_<Engine>(m, "Engine",
                       "Replays columnar market data through the simulator with a no-op strategy.")
        .def(py::init<const py::list&, std::int64_t, std::int64_t>(), py::arg("instruments"),
             py::arg("latency_ns") = 1, py::arg("start_ns") = 0)
        .def("add_feed", &Engine::add_feed, py::arg("kind"), py::arg("instrument_id"),
             py::arg("chunks"),
             "Attach a feed: `chunks` yields tuples of numpy arrays (see finforge.data.replay).")
        .def("run", &Engine::run, py::arg("record") = false,
             "Run to completion. Returns the run status name.")
        .def("counts", &Engine::counts, "Market data events seen, per kind.")
        .def("recorded", &Engine::recorded,
             "With run(record=True): ts, instrument_id and kind (0 bar, 1 quote, 2 trade) of "
             "every market data event, in delivery order.")
        .def("journal_hash", &Engine::journal_hash, "SHA-256 of the run journal (DET-1).");

    register_backtest(m);
}
