#include "finforge/order.hpp"

#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "finforge/assert.hpp"

namespace finforge {

namespace detail {

void check_failed(const char* expr, const char* msg, const char* file, int line) {
    std::fprintf(stderr, "finforge: check failed: %s (%s) at %s:%d\n", expr, msg, file, line);
    std::abort();
}

}  // namespace detail

std::string_view to_string(Side s) { return s == Side::Buy ? "BUY" : "SELL"; }

std::string_view to_string(OrderType t) { return t == OrderType::Market ? "MARKET" : "LIMIT"; }

std::string_view to_string(Tif t) {
    switch (t) {
        case Tif::Gtc:
            return "GTC";
        case Tif::Ioc:
            return "IOC";
        case Tif::Fok:
            return "FOK";
    }
    return "?";
}

std::string_view to_string(OrderStatus s) {
    switch (s) {
        case OrderStatus::New:
            return "NEW";
        case OrderStatus::Accepted:
            return "ACCEPTED";
        case OrderStatus::PartiallyFilled:
            return "PARTIALLY_FILLED";
        case OrderStatus::Filled:
            return "FILLED";
        case OrderStatus::Cancelled:
            return "CANCELLED";
        case OrderStatus::Rejected:
            return "REJECTED";
    }
    return "?";
}

std::string_view to_string(RejectReason r) {
    switch (r) {
        case RejectReason::None:
            return "NONE";
        case RejectReason::InvalidQty:
            return "INVALID_QTY";
        case RejectReason::InvalidPrice:
            return "INVALID_PRICE";
        case RejectReason::OffTick:
            return "OFF_TICK";
        case RejectReason::OffLot:
            return "OFF_LOT";
        case RejectReason::UnknownInstrument:
            return "UNKNOWN_INSTRUMENT";
        case RejectReason::InsufficientCash:
            return "INSUFFICIENT_CASH";
        case RejectReason::PositionLimit:
            return "POSITION_LIMIT";
        case RejectReason::LeverageLimit:
            return "LEVERAGE_LIMIT";
        case RejectReason::ShortNotAllowed:
            return "SHORT_NOT_ALLOWED";
        case RejectReason::UnknownOrder:
            return "UNKNOWN_ORDER";
        case RejectReason::AlreadyTerminal:
            return "ALREADY_TERMINAL";
    }
    return "?";
}

std::string_view to_string(CancelReason r) {
    switch (r) {
        case CancelReason::None:
            return "NONE";
        case CancelReason::User:
            return "USER";
        case CancelReason::IocRemainder:
            return "IOC_REMAINDER";
        case CancelReason::FokUnfillable:
            return "FOK_UNFILLABLE";
        case CancelReason::MarketRemainder:
            return "MARKET_REMAINDER";
        case CancelReason::EndOfRun:
            return "END_OF_RUN";
    }
    return "?";
}

bool is_legal_transition(OrderStatus from, OrderStatus to) {
    using S = OrderStatus;
    switch (from) {
        case S::New:
            return to == S::Accepted || to == S::Rejected;
        case S::Accepted:
            return to == S::PartiallyFilled || to == S::Filled || to == S::Cancelled;
        case S::PartiallyFilled:
            return to == S::PartiallyFilled || to == S::Filled || to == S::Cancelled;
        case S::Filled:
        case S::Cancelled:
        case S::Rejected:
            return false;  // ORD-3: terminal states never change
    }
    return false;
}

void transition(Order& o, OrderStatus to) {
    FINFORGE_CHECK(is_legal_transition(o.status, to), "illegal order transition (ORD-1)");
    o.status = to;
}

}  // namespace finforge
