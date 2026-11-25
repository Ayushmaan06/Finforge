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

#include "finforge/execution_model.hpp"
#include "finforge/instrument.hpp"
#include "finforge/journal.hpp"
#include "finforge/replay.hpp"
#include "finforge/simulator.hpp"
#include "finforge/strategy.hpp"
#include "finforge/version.hpp"

namespace py = pybind11;
using namespace finforge;

namespace {

FeedKind parse_kind(const std::string& kind) {
    if (kind == "bars") return FeedKind::Bars;
    if (kind == "quotes") return FeedKind::Quotes;
    if (kind == "trades") return FeedKind::Trades;
    throw py::value_error("feed kind must be 'bars', 'quotes' or 'trades', not '" + kind + "'");
}

// Pulls chunks from a Python iterator of array tuples.
class PyChunkProvider final : public ChunkProvider {
  public:
    PyChunkProvider(FeedKind kind, const py::iterable& chunks)
        : kind_(kind), iter_(py::iter(chunks)) {}

    bool next(ColumnChunk& out) override {
        PyObject* raw = PyIter_Next(iter_.ptr());
        if (raw == nullptr) {
            if (PyErr_Occurred() != nullptr) throw py::error_already_set();
            return false;
        }
        auto item = std::make_shared<py::object>(py::reinterpret_steal<py::object>(raw));
        const py::sequence cols = item->cast<py::sequence>();
        const std::size_t n_int = column_count(kind_);
        const std::size_t n_total = n_int + (kind_ == FeedKind::Trades ? 1 : 0);
        if (cols.size() != n_total) {
            throw py::value_error("chunk must have " + std::to_string(n_total) + " columns, got " +
                                  std::to_string(cols.size()));
        }
        out = ColumnChunk{};
        bool first = true;
        auto check = [&](const py::handle& h, const py::dtype& want, std::size_t c) -> const void* {
            if (!py::isinstance<py::array>(h)) {
                throw py::type_error("column " + std::to_string(c) + " is not a numpy array");
            }
            const auto a = py::reinterpret_borrow<py::array>(h);
            if (!a.dtype().is(want) || a.ndim() != 1 || (a.flags() & py::array::c_style) == 0) {
                throw py::type_error(
                    "column " + std::to_string(c) + " must be a 1-D C-contiguous " +
                    py::str(static_cast<py::handle>(want)).cast<std::string>() + " array, got " +
                    py::str(static_cast<py::handle>(a.dtype())).cast<std::string>());
            }
            const auto len = static_cast<std::size_t>(a.shape(0));
            if (first) {
                out.rows = len;
                first = false;
            } else if (len != out.rows) {
                throw py::value_error("chunk columns differ in length");
            }
            return a.data();
        };
        for (std::size_t c = 0; c < n_int; ++c) {
            out.cols[c] =
                static_cast<const std::int64_t*>(check(cols[c], py::dtype::of<std::int64_t>(), c));
        }
        if (kind_ == FeedKind::Trades) {
            out.aggressor = static_cast<const std::int8_t*>(
                check(cols[n_int], py::dtype::of<std::int8_t>(), n_int));
        }
        // The chunk owns a reference to the Python tuple; release it under the GIL.
        out.keepalive = std::shared_ptr<const void>(item.get(), [item](const void*) mutable {
            py::gil_scoped_acquire gil;
            item.reset();
        });
        return true;
    }

  private:
    FeedKind kind_;
    py::iterator iter_;
};

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

template <class T>
py::array_t<T> to_numpy(const std::vector<T>& v) {
    py::array_t<T> a(static_cast<py::ssize_t>(v.size()));
    std::copy(v.begin(), v.end(), a.mutable_data());
    return a;
}

// A replay run: registry + feeds + a no-op strategy (PLAN Phase 3 task 8).
class Engine {
  public:
    Engine(const py::list& instruments, std::int64_t latency_ns, std::int64_t start_ns) {
        InstrumentRegistry reg;
        for (std::size_t i = 0; i < instruments.size(); ++i) {
            const auto d = instruments[i].cast<py::dict>();
            InstrumentSpec s;
            s.symbol = d["symbol"].cast<std::string>();
            if (d.contains("asset_class")) s.asset_class = d["asset_class"].cast<std::string>();
            s.quote_currency = d["quote_currency"].cast<std::string>();
            s.tick_size = d["tick_size"].cast<std::string>();
            s.lot_size = d["lot_size"].cast<std::string>();
            if (d.contains("min_qty")) s.min_qty = Qty{d["min_qty"].cast<std::int64_t>()};
            if (d.contains("contract_multiplier")) {
                s.contract_multiplier = d["contract_multiplier"].cast<std::int64_t>();
            }
            if (d.contains("instrument_id") && d["instrument_id"].cast<std::size_t>() != i) {
                throw py::value_error("instrument_id must be dense from 0 in list order (INS-1)");
            }
            reg.add(std::move(s));
        }
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
}
