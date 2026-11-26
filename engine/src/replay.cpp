#include "finforge/replay.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace finforge {

namespace {

const char* kind_name(FeedKind kind) {
    switch (kind) {
        case FeedKind::Bars:
            return "bars";
        case FeedKind::Quotes:
            return "quotes";
        case FeedKind::Trades:
            return "trades";
    }
    return "?";
}

}  // namespace

// One (kind, instrument) stream: walks the rows of the current chunk, pulling the next chunk from
// the provider when it runs out.
class Replay::Feed {
  public:
    Feed(FeedKind kind, InstrumentId instrument, ChunkProvider& provider)
        : kind_(kind), instrument_(instrument), provider_(provider) {}

    // Moves to the next row. False when the feed is exhausted.
    bool advance() {
        ++pos_;
        while (pos_ >= chunk_.rows) {
            chunk_ = ColumnChunk{};  // release the previous chunk first: bounded memory
            pos_ = 0;
            if (!provider_.next(chunk_)) {
                chunk_ = ColumnChunk{};
                return false;
            }
            check_chunk();
        }
        load();
        ++row_;
        return true;
    }

    [[nodiscard]] Timestamp time() const { return time_; }
    [[nodiscard]] InstrumentId instrument() const { return instrument_; }
    [[nodiscard]] FeedKind kind() const { return kind_; }
    [[nodiscard]] const MarketData& current() const { return current_; }

  private:
    [[noreturn]] void fail(const std::string& what) const {
        throw std::invalid_argument(std::string("feed ") + kind_name(kind_) + " instrument " +
                                    std::to_string(instrument_.value()) + ": " + what);
    }

    void check_chunk() const {
        if (chunk_.rows == 0) return;
        for (std::size_t c = 0; c < column_count(kind_); ++c) {
            if (chunk_.cols[c] == nullptr) fail("missing column " + std::to_string(c));
        }
        if (kind_ == FeedKind::Trades && chunk_.aggressor == nullptr) {
            fail("missing aggressor column");
        }
    }

    void load() {
        const std::size_t i = pos_;
        auto col = [&](std::size_t c) { return chunk_.cols[c][i]; };
        switch (kind_) {
            case FeedKind::Bars: {
                Bar b{instrument_,   Timestamp{col(0)}, Timestamp{col(1)}, Price{col(2)},
                      Price{col(3)}, Price{col(4)},     Price{col(5)},     Qty{col(6)}};
                if (b.close_time <= b.open_time) fail("bar with ts_close <= ts_open");
                current_ = b;
                break;
            }
            case FeedKind::Quotes:
                current_ = Quote{instrument_, Timestamp{col(0)}, Price{col(1)},
                                 Qty{col(2)}, Price{col(3)},     Qty{col(4)}};
                break;
            case FeedKind::Trades: {
                const std::int8_t a = chunk_.aggressor[i];
                if (a < -1 || a > 1) fail("aggressor must be -1, 0 or 1");
                current_ = TradePrint{instrument_,
                                      Timestamp{col(0)},
                                      Price{col(1)},
                                      Qty{col(2)},
                                      a == 1 ? Side::Sell : Side::Buy,
                                      a != -1};
                break;
            }
        }
        const Timestamp t = emit_time(current_);
        if (row_ > 0 && t < time_) {
            fail("emit times go backwards at row " + std::to_string(row_));
        }
        time_ = t;
    }

    FeedKind kind_;
    InstrumentId instrument_;
    ChunkProvider& provider_;
    ColumnChunk chunk_;
    std::size_t pos_ = static_cast<std::size_t>(-1);  // advance() wraps it to 0
    std::uint64_t row_ = 0;                           // rows loaded so far
    Timestamp time_;
    MarketData current_;
};

Replay::Replay() = default;
Replay::~Replay() = default;

void Replay::add_feed(FeedKind kind, InstrumentId instrument, ChunkProvider& provider) {
    if (started_) throw std::logic_error("add_feed after the replay started");
    feeds_.push_back(std::make_unique<Feed>(kind, instrument, provider));
}

bool Replay::before(std::size_t a, std::size_t b) const {
    const Feed& fa = *feeds_[a];
    const Feed& fb = *feeds_[b];
    if (fa.time() != fb.time()) return fa.time() < fb.time();
    if (fa.instrument() != fb.instrument()) return fa.instrument() < fb.instrument();
    // EVT-2: feeds of one instrument are bars < quotes < trades, whatever order they were attached
    // in (FeedKind's values are that order). Attach order only breaks ties between feeds of the
    // same kind and instrument.
    if (fa.kind() != fb.kind()) return fa.kind() < fb.kind();
    return a < b;
}

bool Replay::next(MarketData& out) {
    // std heap functions build a max-heap, so the comparator is reversed.
    auto cmp = [this](std::size_t a, std::size_t b) { return before(b, a); };
    if (!started_) {
        started_ = true;
        for (std::size_t i = 0; i < feeds_.size(); ++i) {
            if (feeds_[i]->advance()) heap_.push_back(i);
        }
        std::make_heap(heap_.begin(), heap_.end(), cmp);
    }
    if (heap_.empty()) return false;

    std::pop_heap(heap_.begin(), heap_.end(), cmp);
    const std::size_t i = heap_.back();
    Feed& f = *feeds_[i];
    out = f.current();
    ++emitted_[static_cast<std::size_t>(f.kind())];
    if (f.advance()) {
        std::push_heap(heap_.begin(), heap_.end(), cmp);
    } else {
        heap_.pop_back();
    }
    return true;
}

}  // namespace finforge
