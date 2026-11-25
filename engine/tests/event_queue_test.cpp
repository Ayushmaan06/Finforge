// EVT-1: the queue pops in (ts, class, instrument_id, seq) order, for every tie-break.

#include "finforge/event_queue.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace finforge;

namespace {

Event of_class(EventClass c) {
    switch (c) {
        case EventClass::Exchange:
            return CancelAction{};
        case EventClass::MarketData:
            return TradePrint{};
        case EventClass::Timer:
            return TimerEvent{};
    }
    return TimerEvent{};
}

std::vector<EventKey> drain(EventQueue& q) {
    std::vector<EventKey> out;
    while (!q.empty()) out.push_back(q.pop().key);
    return out;
}

constexpr EventClass kClasses[] = {EventClass::Exchange, EventClass::MarketData, EventClass::Timer};

}  // namespace

TEST(EventQueue, ClassOfEachEventType) {
    EXPECT_EQ(class_of(SubmitAction{}), EventClass::Exchange);
    EXPECT_EQ(class_of(CancelAction{}), EventClass::Exchange);
    EXPECT_EQ(class_of(AmendAction{}), EventClass::Exchange);
    EXPECT_EQ(class_of(Bar{}), EventClass::MarketData);
    EXPECT_EQ(class_of(Quote{}), EventClass::MarketData);
    EXPECT_EQ(class_of(TradePrint{}), EventClass::MarketData);
    EXPECT_EQ(class_of(TimerEvent{}), EventClass::Timer);
    EXPECT_EQ(class_of(SnapshotEvent{}), EventClass::Timer);
}

TEST(EventQueue, EVT1_EarlierTimestampFirst) {
    EventQueue q;
    q.push(Timestamp{20}, InstrumentId{0}, CancelAction{});
    q.push(Timestamp{10}, InstrumentId{0}, TimerEvent{});  // later class, earlier time
    const auto k = drain(q);
    EXPECT_EQ(k[0].ts, Timestamp{10});
    EXPECT_EQ(k[1].ts, Timestamp{20});
}

// Every pair of classes at the same timestamp, pushed in both orders.
TEST(EventQueue, EVT1_SameTimestampEveryClassPair) {
    for (EventClass a : kClasses) {
        for (EventClass b : kClasses) {
            EventQueue q;
            q.push(Timestamp{5}, InstrumentId{0}, of_class(a));
            q.push(Timestamp{5}, InstrumentId{0}, of_class(b));
            const auto k = drain(q);
            ASSERT_EQ(k.size(), 2u);
            EXPECT_LE(k[0].cls, k[1].cls);
            if (a == b) {
                EXPECT_LT(k[0].seq, k[1].seq);  // same class: insertion order
            }
        }
    }
}

TEST(EventQueue, EVT1_AllClassesAtOneTimestamp) {
    EventQueue q;
    q.push(Timestamp{1}, InstrumentId{0}, SnapshotEvent{});
    q.push(Timestamp{1}, InstrumentId{0}, Quote{});
    q.push(Timestamp{1}, InstrumentId{0}, TimerEvent{});
    q.push(Timestamp{1}, InstrumentId{0}, SubmitAction{});
    q.push(Timestamp{1}, InstrumentId{0}, Bar{});
    q.push(Timestamp{1}, InstrumentId{0}, AmendAction{});
    std::vector<EventClass> classes;
    std::vector<std::uint64_t> seqs;
    for (const auto& k : drain(q)) {
        classes.push_back(k.cls);
        seqs.push_back(k.seq.value());
    }
    using C = EventClass;
    EXPECT_EQ(classes, (std::vector<C>{C::Exchange, C::Exchange, C::MarketData, C::MarketData,
                                       C::Timer, C::Timer}));
    EXPECT_EQ(seqs, (std::vector<std::uint64_t>{4, 6, 2, 5, 1, 3}));
}

// EVT-2: equal timestamp and class → lower instrument id first, then insertion order.
TEST(EventQueue, EVT2_InstrumentThenSeq) {
    EventQueue q;
    q.push(Timestamp{1}, InstrumentId{2}, Quote{});
    q.push(Timestamp{1}, InstrumentId{0}, Quote{});
    q.push(Timestamp{1}, InstrumentId{2}, Quote{});
    q.push(Timestamp{1}, InstrumentId{1}, Quote{});
    std::vector<std::pair<std::uint32_t, std::uint64_t>> got;
    for (const auto& k : drain(q)) got.emplace_back(k.instrument.value(), k.seq.value());
    EXPECT_EQ(got, (std::vector<std::pair<std::uint32_t, std::uint64_t>>{
                       {0, 2}, {1, 4}, {2, 1}, {2, 3}}));
}

TEST(EventQueue, EVT1_KeyIsATotalOrder) {
    EventQueue q;
    for (int i = 0; i < 1000; ++i) {
        q.push(Timestamp{i % 7}, InstrumentId{static_cast<std::uint32_t>(i % 3)},
               of_class(kClasses[i % 3]));
    }
    const auto k = drain(q);
    for (std::size_t i = 1; i < k.size(); ++i) ASSERT_LT(k[i - 1], k[i]);
}
