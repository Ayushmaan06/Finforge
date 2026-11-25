// Simulator tests: ordering through the dispatcher (EVT-1/2), latency (EVT-3), strictly increasing
// keys (EVT-4), progress and cancel hooks, the journal format (OP-2) and determinism (DET-1,
// DET-3).

#include "finforge/simulator.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "finforge/execution_model.hpp"
#include "finforge/journal.hpp"
#include "finforge/sha256.hpp"
#include "finforge/synthetic.hpp"

using namespace finforge;

namespace {

InstrumentRegistry two_instruments() {
    InstrumentRegistry r;
    r.add({.symbol = "A", .quote_currency = "USD", .tick_size = "1", .lot_size = "1"});
    r.add({.symbol = "B", .quote_currency = "USD", .tick_size = "1", .lot_size = "1"});
    return r;
}

// A strategy built from lambdas, recording what it sees as "<time>:<what>" strings.
struct Probe : Strategy {
    std::vector<std::string> log;
    std::function<void(Context&)> start;
    std::function<void(Context&, std::uint64_t)> timer;
    std::function<void(Context&, const OwnOrderUpdate&)> update;

    void on_start(Context& c) override {
        if (start) start(c);
    }
    void on_timer(Context& c, std::uint64_t tag) override {
        log.push_back(std::to_string(c.now().value()) + ":timer" + std::to_string(tag));
        if (timer) timer(c, tag);
    }
    void on_market_data(Context& c, const MarketData& md) override {
        log.push_back(std::to_string(c.now().value()) + ":md" +
                      std::to_string(instrument_of(md).value()));
    }
    void on_order_update(Context& c, const OwnOrderUpdate& u) override {
        log.push_back(std::to_string(c.now().value()) + ":" +
                      std::string(to_string(u.update.status)) + "#" + std::to_string(u.client_ref));
        if (update) update(c, u);
    }
    void on_fill(Context& c, const Trade& t) override {
        log.push_back(std::to_string(c.now().value()) + ":fill" + std::to_string(t.qty.value()));
    }
    void on_action_refused(Context& c, OrderId id, RejectReason r) override {
        log.push_back(std::to_string(c.now().value()) + ":refused" + std::to_string(id.value()) +
                      std::string(to_string(r)));
    }
};

OrderRequest limit(InstrumentId inst, Side side, std::int64_t px, std::int64_t q) {
    return OrderRequest{inst, side, OrderType::Limit, Tif::Gtc, Price{px}, Qty{q}};
}

struct Harness {
    BookModel model{two_instruments()};
    Journal journal;
    std::unique_ptr<Simulator> sim;
    explicit Harness(SimConfig c = {}) : sim(std::make_unique<Simulator>(model, journal, c)) {}
};

}  // namespace

// --- Ordering through the dispatcher ------------------------------------------------------------

// At one timestamp: the order arrival (class 0), then market data (1), then the timer (2), even
// though they were scheduled in the opposite order.
TEST(Simulator, EVT1_ClassesAtSameTimestampDispatchInOrder) {
    Harness h;
    Probe p;
    p.start = [&](Context& c) {
        c.set_timer(Timestamp{10}, 7);  // class 2 at t=10, scheduled first
    };
    h.sim->add_strategy(p);
    h.sim->schedule_market_data(Quote{InstrumentId{0}, Timestamp{10}, Price{1}, Qty{1}, Price{2},
                                      Qty{1}});  // class 1 at t=10
    // A second strategy's order is submitted at t=9 and arrives at t=10 (latency 1): class 0.
    Probe q;
    q.start = [&](Context& c) { c.set_timer(Timestamp{9}, 0); };
    q.timer = [&](Context& c, std::uint64_t) { c.submit(limit(InstrumentId{0}, Side::Buy, 5, 1)); };
    h.sim->add_strategy(q);
    EXPECT_EQ(h.sim->run(), RunStatus::Completed);
    EXPECT_EQ(q.log, (std::vector<std::string>{"9:timer0", "10:ACCEPTED#1", "10:md0"}));
    EXPECT_EQ(p.log, (std::vector<std::string>{"10:md0", "10:timer7"}));
}

// EVT-2: same timestamp and class → lower instrument id first, whatever the scheduling order.
TEST(Simulator, EVT2_MarketDataMergedByInstrument) {
    Harness h;
    Probe p;
    h.sim->add_strategy(p);
    h.sim->schedule_market_data(TradePrint{InstrumentId{1}, Timestamp{5}, Price{1}, Qty{1}});
    h.sim->schedule_market_data(TradePrint{InstrumentId{0}, Timestamp{5}, Price{1}, Qty{1}});
    h.sim->schedule_market_data(Bar{InstrumentId{1}, Timestamp{0}, Timestamp{4}, Price{1}, Price{1},
                                    Price{1}, Price{1}, Qty{1}});  // TIME-2: emitted at close_time
    h.sim->run();
    EXPECT_EQ(p.log, (std::vector<std::string>{"4:md1", "5:md0", "5:md1"}));
}

// --- Latency (EVT-3) ----------------------------------------------------------------------------

// A strategy sees a resting ask at t=100 and sends a market buy. The seller's cancel was sent at
// t=95 and arrives at t=105, before the buy (t=110): the buyer cannot trade against the book it
// observed, only against the book as it is when its order arrives.
TEST(Simulator, EVT3_OrderArrivesAfterLatencyAndCannotTradeOnWhatItSaw) {
    SimConfig cfg;
    cfg.latency_ns = 10;
    Harness h(cfg);
    Probe seller, buyer;
    OrderId ask{};
    seller.start = [&](Context& c) {
        c.submit(limit(InstrumentId{0}, Side::Sell, 100, 5));  // arrives t=10
        c.set_timer(Timestamp{95}, 1);
    };
    seller.update = [&](Context&, const OwnOrderUpdate& u) {
        if (u.update.status == OrderStatus::Accepted) ask = u.update.order;
    };
    seller.timer = [&](Context& c, std::uint64_t) { c.cancel(ask); };
    bool saw_ask = false;
    buyer.start = [&](Context& c) { c.set_timer(Timestamp{100}, 1); };
    buyer.timer = [&](Context& c, std::uint64_t) {
        saw_ask = c.book(InstrumentId{0})->best_ask() == Price{100};
        c.submit(OrderRequest{InstrumentId{0}, Side::Buy, OrderType::Market, Tif::Gtc, Price{0},
                              Qty{5}});
    };
    h.sim->add_strategy(seller);
    h.sim->add_strategy(buyer);
    h.sim->run();
    EXPECT_TRUE(saw_ask);
    EXPECT_EQ(seller.log,
              (std::vector<std::string>{"10:ACCEPTED#1", "95:timer1", "105:CANCELLED#1"}));
    EXPECT_EQ(buyer.log, (std::vector<std::string>{"100:timer1", "110:ACCEPTED#2",
                                                   "110:CANCELLED#2"}));  // MARKET_REMAINDER
    EXPECT_EQ(h.model.engine().trade_count(), 0u);
}

TEST(Simulator, EVT3_LatencyMustBePositive) {
    BookModel m{two_instruments()};
    Journal j;
    SimConfig cfg;
    cfg.latency_ns = 0;
    EXPECT_THROW(Simulator(m, j, cfg), std::invalid_argument);
}

TEST(Simulator, EVT4_TimerMustBeInTheFuture) {
    Harness h;
    Probe p;
    bool threw = false;
    p.start = [&](Context& c) { c.set_timer(Timestamp{5}, 0); };
    p.timer = [&](Context& c, std::uint64_t) {
        try {
            c.set_timer(c.now(), 1);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
    };
    h.sim->add_strategy(p);
    h.sim->run();
    EXPECT_TRUE(threw);
}

TEST(Simulator, MarketDataCannotBeScheduledInThePast) {
    Harness h;
    h.sim->schedule_market_data(TradePrint{InstrumentId{0}, Timestamp{10}, Price{1}, Qty{1}});
    h.sim->run();
    EXPECT_THROW(
        h.sim->schedule_market_data(TradePrint{InstrumentId{0}, Timestamp{10}, Price{1}, Qty{1}}),
        std::invalid_argument);
}

// --- Ownership, client refs, refusals -----------------------------------------------------------

TEST(Simulator, CannotCancelSomeoneElsesOrder) {
    Harness h;
    Probe a, b;
    a.start = [&](Context& c) { c.submit(limit(InstrumentId{0}, Side::Buy, 50, 1)); };
    b.start = [&](Context& c) { c.set_timer(Timestamp{5}, 0); };
    b.timer = [&](Context& c, std::uint64_t) {
        c.cancel(OrderId{1});   // a's order
        c.cancel(OrderId{99});  // nobody's
    };
    h.sim->add_strategy(a);
    h.sim->add_strategy(b);
    h.sim->run();
    EXPECT_EQ(b.log, (std::vector<std::string>{"5:timer0", "6:refused1UNKNOWN_ORDER",
                                               "6:refused99UNKNOWN_ORDER"}));
    EXPECT_EQ(h.model.order(OrderId{1})->status, OrderStatus::Accepted);
}

TEST(Simulator, FillsReachBothOwnersAndClientRefSurvivesReplace) {
    Harness h;
    Probe a, b;
    a.start = [&](Context& c) {
        c.submit(limit(InstrumentId{0}, Side::Sell, 101, 3));  // ref 1, arrives t=1
        c.set_timer(Timestamp{5}, 0);
    };
    a.timer = [&](Context& c, std::uint64_t) { c.amend(OrderId{1}, Qty{3}, Price{100}); };
    b.start = [&](Context& c) { c.set_timer(Timestamp{10}, 0); };
    b.timer = [&](Context& c, std::uint64_t) {
        c.submit(limit(InstrumentId{0}, Side::Buy, 100, 2));  // ref 2
    };
    h.sim->add_strategy(a);
    h.sim->add_strategy(b);
    h.sim->run();
    EXPECT_EQ(a.log,
              (std::vector<std::string>{"1:ACCEPTED#1", "5:timer0", "6:CANCELLED#1", "6:ACCEPTED#1",
                                        "11:fill2", "11:PARTIALLY_FILLED#1"}));
    EXPECT_EQ(b.log,
              (std::vector<std::string>{"10:timer0", "11:ACCEPTED#2", "11:fill2", "11:FILLED#2"}));
}

// --- Run control --------------------------------------------------------------------------------

TEST(Simulator, StepAndRunUntil) {
    Harness h;
    for (int t : {10, 20, 30}) {
        h.sim->schedule_market_data(TradePrint{InstrumentId{0}, Timestamp{t}, Price{1}, Qty{1}});
    }
    EXPECT_TRUE(h.sim->step());
    EXPECT_EQ(h.sim->now(), Timestamp{10});
    EXPECT_EQ(h.sim->run_until(Timestamp{25}), RunStatus::ReachedTime);
    EXPECT_EQ(h.sim->events_processed(), 2u);
    EXPECT_EQ(h.sim->now(), Timestamp{25});  // clock advanced to the limit
    EXPECT_EQ(h.sim->run_until(Timestamp{100}), RunStatus::Completed);
    EXPECT_FALSE(h.sim->step());
    EXPECT_EQ(h.sim->events_processed(), 3u);
}

TEST(Simulator, EVT4_ProcessedKeysStrictlyIncrease) {
    SyntheticRunConfig cfg;
    cfg.seed = 3;
    SyntheticMarket m(cfg);
    EventKey prev = m.sim().last_key();
    for (int i = 0; i < 50'000; ++i) {
        ASSERT_TRUE(m.sim().step());
        ASSERT_TRUE(i == 0 || prev < m.sim().last_key());
        ASSERT_LE(prev.ts, m.sim().now());  // TIME-5
        prev = m.sim().last_key();
    }
}

TEST(Simulator, ProgressCallbackEveryN) {
    std::vector<std::uint64_t> seen;
    SyntheticRunConfig cfg;
    cfg.max_events = 10'500;
    cfg.progress_every = 1'000;
    cfg.on_progress = [&](const SimProgress& p) {
        seen.push_back(p.events);
        EXPECT_GT(p.queued, 0u);
    };
    const auto r = run_synthetic(cfg);
    EXPECT_EQ(r.events, 10'500u);
    ASSERT_EQ(seen.size(), 10u);
    for (std::size_t i = 0; i < seen.size(); ++i) EXPECT_EQ(seen[i], 1'000 * (i + 1));
}

// A cancel requested from the progress callback stops the run within cancel_check_every events,
// between two events: the book is consistent and the run can resume to the same final journal as
// an uninterrupted run.
TEST(Simulator, CancelStopsWithinNEventsAndStateIsConsistent) {
    SyntheticRunConfig base;
    base.seed = 11;
    base.max_events = 30'000;
    const std::string uninterrupted = run_synthetic(base).journal_sha256;

    std::atomic<bool> cancel{false};
    SyntheticRunConfig cfg = base;
    cfg.cancel_flag = &cancel;
    cfg.cancel_check_every = 256;
    cfg.progress_every = 1;
    cfg.on_progress = [&](const SimProgress& p) {
        if (p.events == 10'001) cancel = true;
    };
    SyntheticMarket m(cfg);
    EXPECT_EQ(m.run(), RunStatus::Cancelled);
    const std::uint64_t stopped_at = m.sim().events_processed();
    EXPECT_GE(stopped_at, 10'001u);
    EXPECT_LE(stopped_at, 10'001u + 256u);
    EXPECT_EQ(stopped_at % 256, 0u);  // checked only at multiples of N
    m.model().engine().check_invariants();

    cancel = false;
    EXPECT_EQ(m.run(base.max_events - stopped_at), RunStatus::ReachedEventLimit);
    EXPECT_EQ(m.sim().events_processed(), base.max_events);
    EXPECT_EQ(m.journal().hash_hex(), uninterrupted);
}

// --- Journal (OP-2) -----------------------------------------------------------------------------

TEST(Journal, HeaderAndRecordBytes) {
    std::ostringstream out;
    Journal j(&out);
    j.record_input(EventKey{Timestamp{0x0102}, EventClass::Exchange, InstrumentId{3}, Seq{4}},
                   CancelAction{OwnerId{5}, OrderId{6}});
    const std::string b = out.str();
    const std::string expected = std::string("FFJ1\x01\x00\x00\x00", 8) +
                                 std::string("\x02\x00\x21\x00", 4) +  // type 2, len 33
                                 std::string("\x02\x01\x00\x00\x00\x00\x00\x00", 8) +  // ts
                                 std::string("\x00", 1) +                              // class
                                 std::string("\x03\x00\x00\x00", 4) +                  // instr
                                 std::string("\x04\x00\x00\x00\x00\x00\x00\x00", 8) +  // seq
                                 std::string("\x05\x00\x00\x00", 4) +                  // owner
                                 std::string("\x06\x00\x00\x00\x00\x00\x00\x00", 8);   // order
    EXPECT_EQ(b, expected);
    EXPECT_EQ(j.bytes(), b.size());
    EXPECT_EQ(j.records(), 1u);
    Sha256 h;
    h.update(b);
    EXPECT_EQ(j.hash_hex(), h.hex());  // the fingerprint is the hash of exactly these bytes
}

TEST(Journal, SinkReceivesEverythingHashed) {
    std::ostringstream out;
    SyntheticRunConfig cfg;
    cfg.max_events = 20'000;
    cfg.journal_sink = &out;
    const auto r = run_synthetic(cfg);
    EXPECT_EQ(out.str().size(), r.journal_bytes);
    Sha256 h;
    h.update(out.str());
    EXPECT_EQ(h.hex(), r.journal_sha256);
}

// --- Determinism (DET-1, DET-3) -----------------------------------------------------------------

TEST(Determinism, DET1_SameSeedSameHashDifferentSeedDifferentHash) {
    SyntheticRunConfig cfg;
    cfg.max_events = 50'000;
    const auto a = run_synthetic(cfg);
    const auto b = run_synthetic(cfg);
    EXPECT_EQ(a.journal_sha256, b.journal_sha256);
    EXPECT_EQ(a.trades, b.trades);
    cfg.seed = 43;
    EXPECT_NE(run_synthetic(cfg).journal_sha256, a.journal_sha256);
}

// The exit criterion: 1M synthetic events give this exact fingerprint in every build (Debug+ASan
// and Release, GCC and Clang: different addresses, different optimisation, DET-3), with the book
// invariants checked throughout. If engine behaviour changes on purpose, update the constant and
// say why in the commit message.
TEST(Determinism, DET1_MillionEventRunMatchesPinnedHash) {
    SyntheticRunConfig cfg;  // seed 42, 1M events, 8 agents: same as `finforge-sim --synthetic`
    cfg.invariant_check_every = 10'000;
    const auto r = run_synthetic(cfg);
    EXPECT_EQ(r.events, 1'000'000u);
    EXPECT_GT(r.trades, 100'000u);
    EXPECT_EQ(r.journal_sha256, "099c56ee5093f3fa3cae82488918732810a458f83ca8eadc779b6f34bcd5a238");
}
