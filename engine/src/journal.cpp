#include "finforge/journal.hpp"

#include <type_traits>
#include <variant>

#include "finforge/assert.hpp"

namespace finforge {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

Journal::Journal(std::ostream* sink) : sink_(sink) {
    buf_ = {'F', 'F', 'J', '1'};
    put_u16(kFormatVersion);
    put_u16(0);
    write(buf_);
}

void Journal::write(std::span<const std::uint8_t> bytes) {
    hash_.update(bytes);
    bytes_ += bytes.size();
    if (sink_ != nullptr) {
        sink_->write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
}

void Journal::begin(RecordType type) {
    buf_.clear();
    put_u8(static_cast<std::uint8_t>(type));
    put_u8(0);
    put_u16(0);  // payload length, patched in finish()
}

void Journal::finish() {
    const std::size_t len = buf_.size() - 4;
    FINFORGE_CHECK(len <= 0xffff, "journal record too long");
    buf_[2] = static_cast<std::uint8_t>(len);
    buf_[3] = static_cast<std::uint8_t>(len >> 8u);
    write(buf_);
    ++records_;
}

void Journal::put_key(const EventKey& key) {
    put_i64(key.ts.value());
    put_u8(static_cast<std::uint8_t>(key.cls));
    put_u32(key.instrument.value());
    put_u64(key.seq.value());
}

void Journal::record_input(const EventKey& key, const Event& event) {
    std::visit(Overloaded{
                   [&](const SubmitAction& a) {
                       begin(RecordType::Submit);
                       put_key(key);
                       put_u32(a.owner.value());
                       put_u64(a.client_ref);
                       put_u32(a.order.instrument.value());
                       put_u8(static_cast<std::uint8_t>(a.order.side));
                       put_u8(static_cast<std::uint8_t>(a.order.type));
                       put_u8(static_cast<std::uint8_t>(a.order.tif));
                       put_i64(a.order.price.value());
                       put_i64(a.order.qty.value());
                   },
                   [&](const CancelAction& a) {
                       begin(RecordType::Cancel);
                       put_key(key);
                       put_u32(a.owner.value());
                       put_u64(a.order.value());
                   },
                   [&](const AmendAction& a) {
                       begin(RecordType::Amend);
                       put_key(key);
                       put_u32(a.owner.value());
                       put_u64(a.order.value());
                       put_i64(a.qty.value());
                       put_i64(a.price.value());
                   },
                   [&](const Bar& b) {
                       begin(RecordType::Bar);
                       put_key(key);
                       put_u32(b.instrument.value());
                       put_i64(b.open_time.value());
                       put_i64(b.close_time.value());
                       put_i64(b.open.value());
                       put_i64(b.high.value());
                       put_i64(b.low.value());
                       put_i64(b.close.value());
                       put_i64(b.volume.value());
                   },
                   [&](const Quote& q) {
                       begin(RecordType::Quote);
                       put_key(key);
                       put_u32(q.instrument.value());
                       put_i64(q.ts.value());
                       put_i64(q.bid.value());
                       put_i64(q.bid_qty.value());
                       put_i64(q.ask.value());
                       put_i64(q.ask_qty.value());
                   },
                   [&](const TradePrint& t) {
                       begin(RecordType::TradePrint);
                       put_key(key);
                       put_u32(t.instrument.value());
                       put_i64(t.ts.value());
                       put_i64(t.price.value());
                       put_i64(t.qty.value());
                       // 0 buy, 1 sell, 2 unknown
                       put_u8(t.aggressor_known ? static_cast<std::uint8_t>(t.aggressor_side) : 2);
                   },
                   [&](const TimerEvent& t) {
                       begin(RecordType::Timer);
                       put_key(key);
                       put_u32(t.owner.value());
                       put_u64(t.tag);
                   },
                   [&](const SnapshotEvent&) {
                       FINFORGE_CHECK(false, "snapshots are recorded with record_snapshot");
                   },
               },
               event);
    finish();
}

void Journal::record_snapshot(const EventKey& key, std::span<const OrderBook* const> books) {
    begin(RecordType::Snapshot);
    put_key(key);
    for (const OrderBook* b : books) {
        put_u32(b->instrument().value());
        const auto bid = b->depth(Side::Buy, 1);
        const auto ask = b->depth(Side::Sell, 1);
        put_u8(bid.empty() ? 0 : 1);
        put_i64(bid.empty() ? 0 : bid[0].price.value());
        put_i64(bid.empty() ? 0 : bid[0].qty.value());
        put_u8(ask.empty() ? 0 : 1);
        put_i64(ask.empty() ? 0 : ask[0].price.value());
        put_i64(ask.empty() ? 0 : ask[0].qty.value());
    }
    finish();
}

void Journal::record_output(const BookEvent& event) {
    std::visit(Overloaded{
                   [&](const OrderUpdate& u) {
                       begin(RecordType::OrderUpdate);
                       put_u64(u.order.value());
                       put_u8(static_cast<std::uint8_t>(u.status));
                       put_i64(u.filled.value());
                       put_i64(u.remaining.value());
                       put_u8(static_cast<std::uint8_t>(u.reject_reason));
                       put_u8(static_cast<std::uint8_t>(u.cancel_reason));
                   },
                   [&](const OrderAmended& a) {
                       begin(RecordType::OrderAmended);
                       put_u64(a.order.value());
                       put_i64(a.qty.value());
                       put_i64(a.remaining.value());
                   },
                   [&](const Trade& t) {
                       begin(RecordType::Trade);
                       put_u64(t.id.value());
                       put_u32(t.instrument.value());
                       put_i64(t.price.value());
                       put_i64(t.qty.value());
                       put_u64(t.aggressor.value());
                       put_u64(t.resting.value());
                       put_u32(t.aggressor_owner.value());
                       put_u32(t.resting_owner.value());
                       put_u8(static_cast<std::uint8_t>(t.aggressor_side));
                       put_i64(t.ts.value());
                   },
               },
               event);
    finish();
}

void Journal::record_refused(const EventKey& key, OwnerId owner, OrderId order,
                             RejectReason reason) {
    begin(RecordType::ActionRefused);
    put_key(key);
    put_u32(owner.value());
    put_u64(order.value());
    put_u8(static_cast<std::uint8_t>(reason));
    finish();
}

}  // namespace finforge
