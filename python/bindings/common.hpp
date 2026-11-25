#pragma once

// Helpers shared by the binding translation units.

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "finforge/instrument.hpp"
#include "finforge/replay.hpp"

namespace finforge::bindings {

namespace py = pybind11;

inline FeedKind parse_kind(const std::string& kind) {
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

// Registry from a list of dicts (finforge.data.Registry.to_dicts()); ids dense in list order.
inline InstrumentRegistry registry_from(const py::list& instruments) {
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
    return reg;
}

template <class T>
py::array_t<T> to_numpy(const std::vector<T>& v) {
    py::array_t<T> a(static_cast<py::ssize_t>(v.size()));
    std::copy(v.begin(), v.end(), a.mutable_data());
    return a;
}

}  // namespace finforge::bindings
