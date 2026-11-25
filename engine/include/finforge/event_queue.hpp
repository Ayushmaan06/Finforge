#pragma once

// Min-priority queue of events by EventKey (EVT-1). A binary heap is not a stable sort on its own,
// but every key carries a unique seq, so "smallest key" is always one well-defined event and the
// pop order is fully deterministic.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "finforge/events.hpp"

namespace finforge {

class EventQueue {
  public:
    // Assigns the next seq (from 1) and returns the full key.
    EventKey push(Timestamp ts, InstrumentId instrument, Event event);

    [[nodiscard]] bool empty() const { return heap_.empty(); }
    [[nodiscard]] std::size_t size() const { return heap_.size(); }
    [[nodiscard]] const QueuedEvent& top() const { return heap_.front(); }
    QueuedEvent pop();

  private:
    struct Later {
        bool operator()(const QueuedEvent& a, const QueuedEvent& b) const { return a.key > b.key; }
    };

    std::vector<QueuedEvent> heap_;  // std::push_heap / pop_heap with Later: front() is smallest
    std::uint64_t next_seq_ = 1;
};

}  // namespace finforge
