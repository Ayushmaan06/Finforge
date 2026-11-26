// `_engine.Backtest`: one Mode A backtest with one Python strategy (PLAN Phase 5, §4.3).
//
// The C++ side owns the execution model, the accounts, the journal and the simulator. A C++
// Strategy (Dispatcher) forwards every callback to a Python handler object with plain integers
// (ticks, lots, Money, ns); finforge.sdk turns them into the user-facing API. Actions are only
// allowed inside a callback, where a simulator Context is current. Python exceptions raised by the
// handler propagate out of run() unchanged; the run cannot continue after that.

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "common.hpp"
#include "finforge/accounts.hpp"
#include "finforge/journal.hpp"
#include "finforge/mode_a.hpp"
#include "finforge/order.hpp"
#include "finforge/replay.hpp"
#include "finforge/simulator.hpp"

namespace finforge::bindings {

namespace {

constexpr OwnerId kOwner{1};

template <class E>
E parse_enum(const std::string& s, std::initializer_list<std::pair<const char*, E>> names,
             const char* what) {
    for (const auto& [n, v] : names) {
        if (s == n) return v;
    }
    throw py::value_error(std::string("unknown ") + what + " '" + s + "'");
}

class Backtest;

class Dispatcher final : public Strategy {
  public:
    explicit Dispatcher(Backtest& bt) : bt_(bt) {}

    void set_handler(const py::object& h) {
        handler_ = h;
        on_start_ = h.attr("on_start");
        on_bar_ = h.attr("on_bar");
        on_quote_ = h.attr("on_quote");
        on_update_ = h.attr("on_order_update");
        on_amended_ = h.attr("on_order_amended");
        on_fill_ = h.attr("on_fill");
    }

    void on_start(Context& c) override;
    void on_market_data(Context& c, const MarketData& md) override;
    void on_order_update(Context& c, const OwnOrderUpdate& u) override;
    void on_order_amended(Context& c, const OrderAmended& a) override;
    void on_fill(Context& c, const Trade& t) override;

    std::unordered_map<std::uint64_t, std::uint64_t> client_ref;  // order id -> client ref

  private:
    Backtest& bt_;
    py::object handler_, on_start_, on_bar_, on_quote_, on_update_, on_amended_, on_fill_;
};

class Backtest {
  public:
    Backtest(const py::list& instruments, const std::string& model,
             const std::string& slippage_kind, std::int64_t slippage_rate,
             std::int64_t participation, std::int64_t latency_ns, std::int64_t start_ns,
             std::int64_t equity_interval_ns, std::uint64_t progress_every)
        : dispatcher_(*this) {
        InstrumentRegistry reg = registry_from(instruments);
        if (model == "candle") {
            CandleConfig cc;
            cc.slippage.kind = parse_enum<SlippageKind>(slippage_kind,
                                                        {{"none", SlippageKind::None},
                                                         {"fixed_bps", SlippageKind::FixedBps},
                                                         {"half_spread", SlippageKind::HalfSpread}},
                                                        "slippage kind");
            cc.slippage.rate = Rate{slippage_rate};
            cc.participation = Rate{participation};
            model_ = std::make_unique<CandleModel>(std::move(reg), cc);
        } else if (model == "quote") {
            if (slippage_kind != "none") {
                throw py::value_error("the quote model has no slippage (SLP-2)");
            }
            if (participation != 0) throw py::value_error("participation applies to candle only");
            model_ = std::make_unique<QuoteModel>(std::move(reg));
        } else {
            throw py::value_error("execution model must be 'candle' or 'quote', not '" + model +
                                  "'");
        }
        accounts_ = std::make_unique<Accounts>(*model_, AccountsConfig{equity_interval_ns});
        SimConfig sc;
        sc.start = Timestamp{start_ns};
        sc.latency_ns = latency_ns;
        // Cancellation stops the run between two events (the simulator checks the flag every
        // 1024 events); progress reports go to an optional Python callback.
        sc.cancel_flag = &cancel_flag_;
        sc.cancel_check_every = 1024;
        sc.progress_every = progress_every;
        sc.on_progress = [this](const SimProgress& p) {
            if (progress_cb_.is_none()) return;
            if (progress_cb_(p.events, p.now.value()).cast<bool>()) request_cancel();
        };
        sim_ = std::make_unique<Simulator>(*model_, journal_, sc);
    }

    void set_handler(const py::object& handler) {
        if (has_handler_) throw std::logic_error("the handler is already set");
        dispatcher_.set_handler(handler);
        const OwnerId owner = sim_->add_strategy(dispatcher_);
        if (owner != kOwner) throw std::logic_error("unexpected owner id");
        has_handler_ = true;
    }

    void open_account(std::int64_t cash, std::int64_t maker, std::int64_t taker,
                      std::int64_t min_fee, bool allow_short, std::int64_t max_position,
                      std::int64_t max_leverage, std::int64_t borrow_rate) {
        accounts_->open(kOwner, AccountConfig{Money{cash},
                                              {Rate{maker}, Rate{taker}, Money{min_fee}},
                                              allow_short,
                                              Qty{max_position},
                                              Rate{max_leverage},
                                              Rate{borrow_rate}});
        has_account_ = true;
    }

    void add_feed(const std::string& kind, std::uint32_t instrument_id,
                  const py::iterable& chunks) {
        if (started_) throw std::logic_error("add_feed after run()");
        if (instrument_id >= model_->instrument_count())
            throw py::value_error("unknown instrument_id");
        providers_.push_back(std::make_unique<PyChunkProvider>(parse_kind(kind), chunks));
        replay_.add_feed(parse_kind(kind), InstrumentId{instrument_id}, *providers_.back());
    }

    // `callback(events_processed, sim_now_ns)` every `progress_every` events; returning True
    // requests cancellation.
    void set_progress(const py::object& callback) { progress_cb_ = callback; }
    void request_cancel() { cancel_flag_.store(true, std::memory_order_relaxed); }

    std::string run() {
        if (!has_handler_ || !has_account_) {
            throw std::logic_error("set_handler and open_account before run()");
        }
        if (started_) throw std::logic_error("a Backtest runs once");
        started_ = true;
        sim_->attach_accounts(*accounts_);
        sim_->attach_source(replay_);
        const RunStatus s = sim_->run();
        if (s == RunStatus::Cancelled) return "cancelled";
        return s == RunStatus::Completed ? "completed" : "stopped";
    }

    void finish() { sim_->finish(); }  // OP-3 + ACC-10/11 (aborts on a ledger bug)

    // --- Actions (inside a callback only) ---

    std::uint64_t submit(std::uint32_t instrument, const std::string& side, const std::string& type,
                         const std::string& tif, std::int64_t price, std::int64_t qty) {
        const OrderRequest r{
            InstrumentId{instrument},
            parse_enum<Side>(side, {{"buy", Side::Buy}, {"sell", Side::Sell}}, "side"),
            parse_enum<OrderType>(
                type, {{"market", OrderType::Market}, {"limit", OrderType::Limit}}, "order type"),
            parse_enum<Tif>(tif, {{"gtc", Tif::Gtc}, {"ioc", Tif::Ioc}, {"fok", Tif::Fok}}, "tif"),
            Price{price},
            Qty{qty}};
        return ctx().submit(r);
    }
    void cancel(std::uint64_t order_id) { ctx().cancel(OrderId{order_id}); }
    void amend(std::uint64_t order_id, std::int64_t qty, std::int64_t price) {
        ctx().amend(OrderId{order_id}, Qty{qty}, Price{price});
    }

    // --- Queries (any time) ---

    [[nodiscard]] std::int64_t now() const { return sim_->now().value(); }
    [[nodiscard]] py::tuple position(std::uint32_t instrument) const {
        const Position& p = ledger().position(InstrumentId{instrument});
        return py::make_tuple(p.qty.value(), p.cost_basis.value());
    }
    [[nodiscard]] std::int64_t mark(std::uint32_t instrument) const {
        return accounts_->mark(InstrumentId{instrument}).value();
    }
    [[nodiscard]] py::dict account() const {
        const Ledger& l = ledger();
        const auto m = accounts_->marks();
        py::dict d;
        d["cash"] = l.cash().value();
        d["reserved_cash"] = l.reserved_cash().value();
        d["equity"] = l.equity(m).value();
        d["realised_pnl"] = l.realised_pnl().value();
        d["unrealised_pnl"] = l.unrealised_pnl(m).value();
        d["total_fees"] = l.total_fees().value();
        d["borrow_costs"] = l.borrow_costs().value();
        d["gross_exposure"] = l.gross_exposure(m).value();
        return d;
    }

    // --- Results ---

    [[nodiscard]] py::dict orders() const {
        std::vector<std::uint64_t> id, ref;
        std::vector<std::uint32_t> inst;
        std::vector<std::uint8_t> side, type, tif, status, reject, cancel;
        std::vector<std::int64_t> price, qty, filled, cancelled, submit_ts, sent_ts;
        for (std::size_t i = 1; i <= model_->order_count(); ++i) {
            const Order& o = *model_->order(OrderId{i});
            id.push_back(i);
            const auto it = dispatcher_.client_ref.find(i);
            ref.push_back(it == dispatcher_.client_ref.end() ? 0 : it->second);
            inst.push_back(o.instrument.value());
            side.push_back(static_cast<std::uint8_t>(o.side));
            type.push_back(static_cast<std::uint8_t>(o.type));
            tif.push_back(static_cast<std::uint8_t>(o.tif));
            status.push_back(static_cast<std::uint8_t>(o.status));
            reject.push_back(static_cast<std::uint8_t>(o.reject_reason));
            cancel.push_back(static_cast<std::uint8_t>(o.cancel_reason));
            price.push_back(o.price.value());
            qty.push_back(o.qty.value());
            filled.push_back(o.filled.value());
            cancelled.push_back(o.cancelled.value());
            submit_ts.push_back(o.submit_ts.value());
            sent_ts.push_back(o.sent_ts.value());
        }
        py::dict d;
        d["order_id"] = to_numpy(id);
        d["client_ref"] = to_numpy(ref);
        d["instrument_id"] = to_numpy(inst);
        d["side"] = to_numpy(side);
        d["type"] = to_numpy(type);
        d["tif"] = to_numpy(tif);
        d["price"] = to_numpy(price);
        d["qty"] = to_numpy(qty);
        d["filled"] = to_numpy(filled);
        d["cancelled"] = to_numpy(cancelled);
        d["status"] = to_numpy(status);
        d["reject_reason"] = to_numpy(reject);
        d["cancel_reason"] = to_numpy(cancel);
        d["submit_ts"] = to_numpy(submit_ts);
        d["sent_ts"] = to_numpy(sent_ts);
        return d;
    }

    [[nodiscard]] py::dict fills() const {
        std::vector<std::int64_t> ts, price, qty, fee;
        std::vector<std::uint64_t> order, trade;
        std::vector<std::uint32_t> inst;
        std::vector<std::uint8_t> side, liq;
        std::vector<std::int64_t> borrow_ts, borrow_amount;
        for (const LedgerEntry& e : ledger().entries()) {
            if (const auto* f = std::get_if<LedgerFill>(&e)) {
                ts.push_back(f->ts.value());
                order.push_back(f->order.value());
                trade.push_back(f->trade.value());
                inst.push_back(f->instrument.value());
                side.push_back(static_cast<std::uint8_t>(f->side));
                price.push_back(f->price.value());
                qty.push_back(f->qty.value());
                fee.push_back(f->fee.value());
                liq.push_back(static_cast<std::uint8_t>(f->liquidity));
            } else {
                const auto& b = std::get<BorrowCharge>(e);
                borrow_ts.push_back(b.ts.value());
                borrow_amount.push_back(b.amount.value());
            }
        }
        py::dict d;
        d["ts"] = to_numpy(ts);
        d["order_id"] = to_numpy(order);
        d["trade_id"] = to_numpy(trade);
        d["instrument_id"] = to_numpy(inst);
        d["side"] = to_numpy(side);
        d["price"] = to_numpy(price);
        d["qty"] = to_numpy(qty);
        d["fee"] = to_numpy(fee);
        d["liquidity"] = to_numpy(liq);
        d["borrow_ts"] = to_numpy(borrow_ts);
        d["borrow_amount"] = to_numpy(borrow_amount);
        return d;
    }

    [[nodiscard]] py::dict equity() const {
        std::vector<std::int64_t> ts, cash, eq, realised, unrealised, fees, borrow, gross;
        for (const EquityPoint& p : accounts_->equity_curve(kOwner)) {
            ts.push_back(p.ts.value());
            cash.push_back(p.cash.value());
            eq.push_back(p.equity.value());
            realised.push_back(p.realised_pnl.value());
            unrealised.push_back(p.unrealised_pnl.value());
            fees.push_back(p.total_fees.value());
            borrow.push_back(p.borrow_costs.value());
            gross.push_back(p.gross_exposure.value());
        }
        py::dict d;
        d["ts"] = to_numpy(ts);
        d["cash"] = to_numpy(cash);
        d["equity"] = to_numpy(eq);
        d["realised_pnl"] = to_numpy(realised);
        d["unrealised_pnl"] = to_numpy(unrealised);
        d["total_fees"] = to_numpy(fees);
        d["borrow_costs"] = to_numpy(borrow);
        d["gross_exposure"] = to_numpy(gross);
        // Per-instrument position (lots) and mark (ticks), shape (points, instruments).
        const auto k = static_cast<py::ssize_t>(model_->instrument_count());
        const auto qty = accounts_->position_curve(kOwner);
        const auto mark = accounts_->mark_curve(kOwner);
        py::array_t<std::int64_t> q({static_cast<py::ssize_t>(ts.size()), k});
        py::array_t<std::int64_t> mk({static_cast<py::ssize_t>(ts.size()), k});
        for (std::size_t i = 0; i < qty.size(); ++i) {
            q.mutable_data()[i] = qty[i].value();
            mk.mutable_data()[i] = mark[i].value();
        }
        d["position"] = q;
        d["mark"] = mk;
        return d;
    }

    [[nodiscard]] std::string journal_hash() const { return journal_.hash_hex(); }
    [[nodiscard]] bool reconciles() const { return accounts_->reconciles(kOwner); }
    [[nodiscard]] py::dict counts() const {
        py::dict d;
        d["events_processed"] = sim_->events_processed();
        d["bars"] = replay_.emitted(FeedKind::Bars);
        d["quotes"] = replay_.emitted(FeedKind::Quotes);
        d["orders"] = model_->order_count();
        d["journal_records"] = journal_.records();
        return d;
    }

    // Scope guard: the simulator context of the callback being dispatched.
    struct Scope {
        Scope(Backtest& b, Context& c) : bt(b), prev(b.ctx_) { b.ctx_ = &c; }
        ~Scope() { bt.ctx_ = prev; }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Backtest& bt;
        Context* prev;
    };

  private:
    Context& ctx() {
        if (ctx_ == nullptr) {
            throw std::logic_error("orders can only be sent from inside a strategy callback");
        }
        return *ctx_;
    }
    [[nodiscard]] const Ledger& ledger() const {
        const Ledger* l = accounts_->ledger(kOwner);
        if (l == nullptr) throw std::logic_error("open_account first");
        return *l;
    }

    std::atomic<bool> cancel_flag_{false};  // before sim_, which points to it
    py::object progress_cb_ = py::none();
    std::unique_ptr<WorkingOrderModel> model_;
    std::unique_ptr<Accounts> accounts_;
    Journal journal_;
    std::unique_ptr<Simulator> sim_;
    std::vector<std::unique_ptr<PyChunkProvider>> providers_;
    Replay replay_;
    Dispatcher dispatcher_;
    Context* ctx_ = nullptr;
    bool has_handler_ = false;
    bool has_account_ = false;
    bool started_ = false;
};

void Dispatcher::on_start(Context& c) {
    const Backtest::Scope s(bt_, c);
    on_start_();
}

void Dispatcher::on_market_data(Context& c, const MarketData& md) {
    const Backtest::Scope s(bt_, c);
    if (const auto* b = std::get_if<Bar>(&md)) {
        on_bar_(b->instrument.value(), b->open_time.value(), b->close_time.value(), b->open.value(),
                b->high.value(), b->low.value(), b->close.value(), b->volume.value());
    } else if (const auto* q = std::get_if<Quote>(&md)) {
        on_quote_(q->instrument.value(), q->ts.value(), q->bid.value(), q->bid_qty.value(),
                  q->ask.value(), q->ask_qty.value());
    }
}

void Dispatcher::on_order_update(Context& c, const OwnOrderUpdate& u) {
    client_ref[u.update.order.value()] = u.client_ref;
    const Backtest::Scope s(bt_, c);
    on_update_(u.client_ref, u.update.order.value(), std::string(to_string(u.update.status)),
               u.update.filled.value(), u.update.remaining.value(),
               std::string(to_string(u.update.reject_reason)),
               std::string(to_string(u.update.cancel_reason)));
}

// ORD-7 in-place reduction: no status change, so no OrderUpdate; the handle still needs it.
void Dispatcher::on_order_amended(Context& c, const OrderAmended& a) {
    const Backtest::Scope s(bt_, c);
    on_amended_(a.order.value(), a.qty.value(), a.remaining.value());
}

void Dispatcher::on_fill(Context& c, const Trade& t) {
    const Backtest::Scope s(bt_, c);
    on_fill_(t.aggressor.value(), t.id.value(), t.instrument.value(),
             std::string(to_string(t.aggressor_side)), t.price.value(), t.qty.value(),
             t.ts.value());
}

py::dict enum_names() {
    auto names = [](auto last, auto fn) {
        py::list l;
        for (int i = 0; i <= static_cast<int>(last); ++i) {
            l.append(std::string(fn(static_cast<decltype(last)>(i))));
        }
        return l;
    };
    py::dict d;
    d["side"] = names(Side::Sell, [](Side v) { return to_string(v); });
    d["type"] = names(OrderType::Limit, [](OrderType v) { return to_string(v); });
    d["tif"] = names(Tif::Fok, [](Tif v) { return to_string(v); });
    d["status"] = names(OrderStatus::Rejected, [](OrderStatus v) { return to_string(v); });
    d["reject_reason"] =
        names(RejectReason::AlreadyTerminal, [](RejectReason v) { return to_string(v); });
    d["cancel_reason"] =
        names(CancelReason::InsufficientCashAtFill, [](CancelReason v) { return to_string(v); });
    d["liquidity"] = py::make_tuple("MAKER", "TAKER");
    return d;
}

}  // namespace

void register_backtest(py::module_& m) {
    m.def("enum_names", &enum_names, "Names of the engine enums, indexed by their numeric value.");
    py::class_<Backtest>(m, "Backtest", "One Mode A backtest driving a Python strategy handler.")
        .def(py::init<const py::list&, const std::string&, const std::string&, std::int64_t,
                      std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::uint64_t>(),
             py::arg("instruments"), py::arg("model"), py::arg("slippage_kind") = "none",
             py::arg("slippage_rate") = 0, py::arg("participation") = 0, py::arg("latency_ns") = 1,
             py::arg("start_ns") = 0, py::arg("equity_interval_ns") = 0,
             py::arg("progress_every") = 0)
        .def("set_progress", &Backtest::set_progress, py::arg("callback"))
        .def("request_cancel", &Backtest::request_cancel)
        .def("set_handler", &Backtest::set_handler, py::arg("handler"))
        .def("open_account", &Backtest::open_account, py::arg("cash"), py::arg("maker"),
             py::arg("taker"), py::arg("min_fee"), py::arg("allow_short"), py::arg("max_position"),
             py::arg("max_leverage"), py::arg("borrow_rate"))
        .def("add_feed", &Backtest::add_feed, py::arg("kind"), py::arg("instrument_id"),
             py::arg("chunks"))
        .def("run", &Backtest::run)
        .def("finish", &Backtest::finish)
        .def("submit", &Backtest::submit, py::arg("instrument_id"), py::arg("side"),
             py::arg("type"), py::arg("tif"), py::arg("price"), py::arg("qty"))
        .def("cancel", &Backtest::cancel, py::arg("order_id"))
        .def("amend", &Backtest::amend, py::arg("order_id"), py::arg("qty"), py::arg("price"))
        .def("now", &Backtest::now)
        .def("position", &Backtest::position, py::arg("instrument_id"))
        .def("mark", &Backtest::mark, py::arg("instrument_id"))
        .def("account", &Backtest::account)
        .def("orders", &Backtest::orders)
        .def("fills", &Backtest::fills)
        .def("equity", &Backtest::equity)
        .def("journal_hash", &Backtest::journal_hash)
        .def("reconciles", &Backtest::reconciles)
        .def("counts", &Backtest::counts);
}

}  // namespace finforge::bindings
