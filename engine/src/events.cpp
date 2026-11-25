#include "finforge/events.hpp"

#include <algorithm>
#include <type_traits>
#include <utility>
#include <variant>

#include "finforge/event_queue.hpp"

namespace finforge {

Timestamp emit_time(const MarketData& md) {
    return std::visit(
        [](const auto& m) {
            if constexpr (std::is_same_v<std::decay_t<decltype(m)>, Bar>) {
                return m.close_time;  // TIME-2
            } else {
                return m.ts;  // TIME-4
            }
        },
        md);
}

InstrumentId instrument_of(const MarketData& md) {
    return std::visit([](const auto& m) { return m.instrument; }, md);
}

EventClass class_of(const Event& e) {
    switch (e.index()) {
        case 0:  // SubmitAction
        case 1:  // CancelAction
        case 2:  // AmendAction
            return EventClass::Exchange;
        case 3:  // Bar
        case 4:  // Quote
        case 5:  // TradePrint
            return EventClass::MarketData;
        default:  // TimerEvent, SnapshotEvent
            return EventClass::Timer;
    }
}

EventKey EventQueue::push(Timestamp ts, InstrumentId instrument, Event event) {
    const EventKey key{ts, class_of(event), instrument, Seq{next_seq_++}};
    heap_.push_back(QueuedEvent{key, std::move(event)});
    std::push_heap(heap_.begin(), heap_.end(), Later{});
    return key;
}

QueuedEvent EventQueue::pop() {
    std::pop_heap(heap_.begin(), heap_.end(), Later{});  // smallest moves to the back
    QueuedEvent e = std::move(heap_.back());
    heap_.pop_back();
    return e;
}

}  // namespace finforge
