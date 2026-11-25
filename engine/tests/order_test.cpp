#include "finforge/order.hpp"

#include <gtest/gtest.h>

#include <array>

using namespace finforge;
using S = OrderStatus;

namespace {
constexpr std::array kAll{S::New,    S::Accepted,  S::PartiallyFilled,
                          S::Filled, S::Cancelled, S::Rejected};
}

// ORD-1: exactly these transitions are legal.
TEST(OrderLifecycle, ORD1_TransitionTable) {
    auto legal = [](S from, S to) {
        switch (from) {
            case S::New:
                return to == S::Accepted || to == S::Rejected;
            case S::Accepted:
            case S::PartiallyFilled:
                return to == S::PartiallyFilled || to == S::Filled || to == S::Cancelled;
            default:
                return false;
        }
    };
    for (S from : kAll) {
        for (S to : kAll) {
            EXPECT_EQ(is_legal_transition(from, to), legal(from, to))
                << to_string(from) << " -> " << to_string(to);
        }
    }
}

// ORD-3: terminal states never change.
TEST(OrderLifecycle, ORD3_TerminalIsFinal) {
    for (S from : {S::Filled, S::Cancelled, S::Rejected}) {
        EXPECT_TRUE(is_terminal(from));
        for (S to : kAll) EXPECT_FALSE(is_legal_transition(from, to));
    }
    EXPECT_FALSE(is_terminal(S::New));
    EXPECT_FALSE(is_terminal(S::Accepted));
    EXPECT_FALSE(is_terminal(S::PartiallyFilled));
}

TEST(OrderLifecycleDeathTest, ORD1_IllegalTransitionTripsCheck) {
    Order o;
    o.status = S::Filled;
    EXPECT_DEATH(transition(o, S::Cancelled), "illegal order transition");
    Order n;
    EXPECT_DEATH(transition(n, S::Filled), "illegal order transition");
}

TEST(OrderLifecycle, LegalTransitionUpdatesStatus) {
    Order o;
    transition(o, S::Accepted);
    transition(o, S::PartiallyFilled);
    transition(o, S::PartiallyFilled);
    transition(o, S::Filled);
    EXPECT_EQ(o.status, S::Filled);
}

TEST(OrderLifecycle, ReasonNames) {
    EXPECT_EQ(to_string(RejectReason::AlreadyTerminal), "ALREADY_TERMINAL");
    EXPECT_EQ(to_string(CancelReason::MarketRemainder), "MARKET_REMAINDER");
    EXPECT_EQ(to_string(S::PartiallyFilled), "PARTIALLY_FILLED");
    EXPECT_EQ(to_string(Tif::Fok), "FOK");
}
