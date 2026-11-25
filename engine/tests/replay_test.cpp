#include "finforge/replay.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <variant>
#include <vector>

#include "finforge/execution_model.hpp"
#include "finforge/journal.hpp"
#include "finforge/simulator.hpp"
#include "test_util.hpp"

namespace finforge {
namespace {

// Column data for one feed, served in chunks of the given sizes (cycled; 0 = an empty chunk).
class VectorProvider final : public ChunkProvider {
  public:
    VectorProvider(std::vector<std::vector<std::int64_t>> cols, std::vector<std::size_t> sizes,
                   std::vector<std::int8_t> aggressor = {})
        : cols_(std::move(cols)), aggressor_(std::move(aggressor)), sizes_(std::move(sizes)) {}

    bool next(ColumnChunk& out) override {
        const std::size_t total = cols_.empty() ? 0 : cols_[0].size();
        if (pos_ >= total) return false;
        const std::size_t n = std::min(sizes_[calls_++ % sizes_.size()], total - pos_);
        out.rows = n;
        for (std::size_t c = 0; c < cols_.size(); ++c) out.cols[c] = cols_[c].data() + pos_;
        if (!aggressor_.empty()) out.aggressor = aggressor_.data() + pos_;
        // Count live chunks to check that the feed releases them.
        out.keepalive = std::make_shared<Live>(live_);
        pos_ += n;
        ++chunks_;
        return true;
    }

    [[nodiscard]] int live() const { return *live_; }
    [[nodiscard]] int chunks() const { return chunks_; }

  private:
    struct Live {
        explicit Live(std::shared_ptr<int> c) : count(std::move(c)) { ++*count; }
        ~Live() { --*count; }
        Live(const Live&) = delete;
        Live& operator=(const Live&) = delete;
        std::shared_ptr<int> count;
    };

    std::vector<std::vector<std::int64_t>> cols_;
    std::vector<std::int8_t> aggressor_;
    std::vector<std::size_t> sizes_;
    std::size_t pos_ = 0;
    std::size_t calls_ = 0;
    int chunks_ = 0;
    std::shared_ptr<int> live_ = std::make_shared<int>(0);
};

VectorProvider quotes(const std::vector<std::int64_t>& ts,
                      std::vector<std::size_t> sizes = {1000}) {
    std::vector<std::int64_t> bid, bq, ask, aq;
    for (std::size_t i = 0; i < ts.size(); ++i) {
        bid.push_back(100 + static_cast<std::int64_t>(i));
        bq.push_back(1);
        ask.push_back(200 + static_cast<std::int64_t>(i));
        aq.push_back(2);
    }
    return VectorProvider({ts, bid, bq, ask, aq}, std::move(sizes));
}

struct Seen {
    std::int64_t ts;
    std::uint32_t instrument;
    std::size_t kind;  // MarketData index: 0 bar, 1 quote, 2 trade
    std::int64_t tag;  // bid for quotes, open for bars, price for trades: identifies the row
};

std::vector<Seen> drain(Replay& r) {
    std::vector<Seen> out;
    MarketData md;
    while (r.next(md)) {
        std::int64_t tag = std::visit(
            [](const auto& m) -> std::int64_t {
                using T = std::decay_t<decltype(m)>;
                if constexpr (std::is_same_v<T, Bar>) return m.open.value();
                if constexpr (std::is_same_v<T, Quote>) return m.bid.value();
                if constexpr (std::is_same_v<T, TradePrint>) return m.price.value();
            },
            md);
        out.push_back({emit_time(md).value(), instrument_of(md).value(), md.index(), tag});
    }
    return out;
}

TEST(Replay, MergesByTimeThenInstrumentThenAttachOrderThenRow) {
    // Instrument 1 is attached first, but instrument 0 wins ties on time.
    auto q1 = quotes({10, 20, 20, 30});
    auto q0 = quotes({5, 20, 20, 40});
    Replay r;
    r.add_feed(FeedKind::Quotes, InstrumentId{1}, q1);
    r.add_feed(FeedKind::Quotes, InstrumentId{0}, q0);
    const auto seen = drain(r);
    ASSERT_EQ(seen.size(), 8U);
    const std::vector<std::pair<std::int64_t, std::uint32_t>> want{
        {5, 0}, {10, 1}, {20, 0}, {20, 0}, {20, 1}, {20, 1}, {30, 1}, {40, 0}};
    for (std::size_t i = 0; i < want.size(); ++i) {
        EXPECT_EQ(seen[i].ts, want[i].first) << i;
        EXPECT_EQ(seen[i].instrument, want[i].second) << i;
    }
    // Rows of one feed keep their order at equal times (bid encodes the row: 100 + row).
    EXPECT_EQ(seen[2].tag, 101);
    EXPECT_EQ(seen[3].tag, 102);
    EXPECT_EQ(seen[4].tag, 101);
    EXPECT_EQ(seen[5].tag, 102);
    EXPECT_EQ(r.emitted(FeedKind::Quotes), 8U);
}

TEST(Replay, SameInstrumentTiesFollowAttachOrder) {
    // A bar closing at 60 and a quote at 60 for the same instrument: bars feed attached first.
    VectorProvider bars({{0}, {60}, {7}, {9}, {6}, {8}, {3}}, {1000});
    auto q = quotes({60});
    VectorProvider trades({{60}, {55}, {4}}, {1000}, {-1});
    Replay r;
    r.add_feed(FeedKind::Bars, InstrumentId{0}, bars);
    r.add_feed(FeedKind::Quotes, InstrumentId{0}, q);
    r.add_feed(FeedKind::Trades, InstrumentId{0}, trades);
    MarketData md;
    ASSERT_TRUE(r.next(md));
    const Bar& b = std::get<Bar>(md);
    EXPECT_EQ(b.open_time, Timestamp{0});
    EXPECT_EQ(b.close_time, Timestamp{60});  // TIME-2: emitted at close time
    EXPECT_EQ(b.high, Price{9});
    EXPECT_EQ(b.volume, Qty{3});
    ASSERT_TRUE(r.next(md));
    EXPECT_TRUE(std::holds_alternative<Quote>(md));
    ASSERT_TRUE(r.next(md));
    const auto& t = std::get<TradePrint>(md);
    EXPECT_EQ(t.price, Price{55});
    EXPECT_FALSE(t.aggressor_known);
    EXPECT_FALSE(r.next(md));
}

TEST(Replay, ChunkBoundariesDoNotChangeTheStream) {
    std::vector<std::int64_t> ts0, ts1;
    for (std::int64_t i = 0; i < 200; ++i) {
        ts0.push_back(i * 3);
        ts1.push_back(i * 2);
    }
    auto reference = [&] {
        auto a = quotes(ts0);
        auto b = quotes(ts1);
        Replay r;
        r.add_feed(FeedKind::Quotes, InstrumentId{0}, a);
        r.add_feed(FeedKind::Quotes, InstrumentId{1}, b);
        return drain(r);
    }();
    ASSERT_EQ(reference.size(), 400U);

    auto a = quotes(ts0, {1, 0, 7, 3, 0, 64});
    auto b = quotes(ts1, {5, 0, 0, 2});
    Replay r;
    r.add_feed(FeedKind::Quotes, InstrumentId{0}, a);
    r.add_feed(FeedKind::Quotes, InstrumentId{1}, b);
    MarketData md;
    std::size_t n = 0;
    while (r.next(md)) {
        // Only the current chunk of each feed is alive (one prefetched row at most).
        EXPECT_LE(a.live(), 1);
        EXPECT_LE(b.live(), 1);
        ASSERT_LT(n, reference.size());
        EXPECT_EQ(emit_time(md).value(), reference[n].ts) << n;
        EXPECT_EQ(instrument_of(md).value(), reference[n].instrument) << n;
        ++n;
    }
    EXPECT_EQ(n, reference.size());
    EXPECT_EQ(a.live(), 0);
    EXPECT_EQ(b.live(), 0);
    EXPECT_GE(a.chunks(), 15);  // many boundaries, including empty chunks
}

TEST(Replay, EmptyFeedsAndNoFeeds) {
    Replay none;
    MarketData md;
    EXPECT_FALSE(none.next(md));

    auto empty = quotes({});
    auto one = quotes({7});
    Replay r;
    r.add_feed(FeedKind::Quotes, InstrumentId{0}, empty);
    r.add_feed(FeedKind::Quotes, InstrumentId{1}, one);
    ASSERT_TRUE(r.next(md));
    EXPECT_EQ(instrument_of(md), InstrumentId{1});
    EXPECT_FALSE(r.next(md));
    EXPECT_THROW(r.add_feed(FeedKind::Quotes, InstrumentId{2}, one), std::logic_error);
}

TEST(Replay, RejectsBadFeeds) {
    {
        auto q = quotes({10, 9}, {1});  // backwards across a chunk boundary
        Replay r;
        r.add_feed(FeedKind::Quotes, InstrumentId{0}, q);
        MarketData md;
        // The first next() already prefetches row 1.
        EXPECT_THROW(
            {
                while (r.next(md)) {
                }
            },
            std::invalid_argument);
    }
    {
        VectorProvider bars({{60}, {60}, {1}, {1}, {1}, {1}, {1}}, {10});  // ts_close == ts_open
        Replay r;
        r.add_feed(FeedKind::Bars, InstrumentId{0}, bars);
        MarketData md;
        EXPECT_THROW(r.next(md), std::invalid_argument);
    }
    {
        VectorProvider trades({{1}, {1}, {1}}, {10}, {5});  // bad aggressor code
        Replay r;
        r.add_feed(FeedKind::Trades, InstrumentId{0}, trades);
        MarketData md;
        EXPECT_THROW(r.next(md), std::invalid_argument);
    }
    {
        VectorProvider trades({{1}, {1}, {1}}, {10});  // aggressor column missing
        Replay r;
        r.add_feed(FeedKind::Trades, InstrumentId{0}, trades);
        MarketData md;
        EXPECT_THROW(r.next(md), std::invalid_argument);
    }
}

// --- Streaming into the simulator ---------------------------------------------------------------

class Recorder final : public Strategy {
  public:
    void on_market_data(Context& ctx, const MarketData& md) override {
        seen.push_back({ctx.now().value(), instrument_of(md).value(), md.index(), 0});
    }
    std::vector<Seen> seen;
};

InstrumentRegistry two_instruments() { return test::make_registry(); }

TEST(SimulatorSource, StreamingMatchesPreScheduledOrderAndClock) {
    std::vector<std::int64_t> ts0{100, 200, 200, 350}, ts1{150, 200, 300};

    // Pre-scheduled: everything in the queue up front.
    std::vector<Seen> pre;
    {
        auto a = quotes(ts0);
        auto b = quotes(ts1);
        Replay r;
        r.add_feed(FeedKind::Quotes, InstrumentId{0}, a);
        r.add_feed(FeedKind::Quotes, InstrumentId{1}, b);
        BookModel model(two_instruments());
        Journal journal;
        Simulator sim(model, journal);
        Recorder rec;
        sim.add_strategy(rec);
        MarketData md;
        while (r.next(md)) sim.schedule_market_data(md);
        EXPECT_EQ(sim.run(), RunStatus::Completed);
        pre = rec.seen;
    }

    auto a = quotes(ts0, {1});
    auto b = quotes(ts1, {2});
    Replay r;
    r.add_feed(FeedKind::Quotes, InstrumentId{0}, a);
    r.add_feed(FeedKind::Quotes, InstrumentId{1}, b);
    BookModel model(two_instruments());
    Journal journal;
    Simulator sim(model, journal, SimConfig{.snapshot_interval_ns = 50});
    Recorder rec;
    sim.add_strategy(rec);
    sim.attach_source(r);
    EXPECT_THROW(sim.attach_source(r), std::logic_error);
    EXPECT_EQ(sim.run(), RunStatus::Completed);

    ASSERT_EQ(rec.seen.size(), 7U);
    ASSERT_EQ(pre.size(), 7U);
    for (std::size_t i = 0; i < pre.size(); ++i) {
        EXPECT_EQ(rec.seen[i].ts, pre[i].ts) << i;  // strategy sees now() == emit time
        EXPECT_EQ(rec.seen[i].instrument, pre[i].instrument) << i;
    }
    // Snapshots keep firing while data is pending: 50..350 = 7 snapshots + 7 quotes.
    EXPECT_EQ(sim.events_processed(), 14U);
    EXPECT_EQ(sim.queued(), 0U);
}

TEST(SimulatorSource, RunUntilStopsBetweenSourceEvents) {
    auto a = quotes({100, 200, 300});
    Replay r;
    r.add_feed(FeedKind::Quotes, InstrumentId{0}, a);
    BookModel model(two_instruments());
    Journal journal;
    Simulator sim(model, journal);
    Recorder rec;
    sim.add_strategy(rec);
    sim.attach_source(r);
    EXPECT_EQ(sim.run_until(Timestamp{250}), RunStatus::ReachedTime);
    EXPECT_EQ(rec.seen.size(), 2U);
    EXPECT_EQ(sim.queued(), 1U);  // only the next source event is queued
    EXPECT_EQ(sim.run(), RunStatus::Completed);
    EXPECT_EQ(rec.seen.size(), 3U);
}

TEST(SimulatorSource, SourceBeforeStartTimeThrows) {
    auto a = quotes({100});
    Replay r;
    r.add_feed(FeedKind::Quotes, InstrumentId{0}, a);
    BookModel model(two_instruments());
    Journal journal;
    Simulator sim(model, journal, SimConfig{.start = Timestamp{500}});
    sim.attach_source(r);
    EXPECT_THROW(sim.run(), std::invalid_argument);
}

}  // namespace
}  // namespace finforge
