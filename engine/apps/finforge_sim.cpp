// finforge-sim: Mode B order-script runner (Demo 2).
//
// Reads an order script (a file argument, or stdin) for one instrument with tick = lot = 1:
//   ts,owner,side,type,tif,price,qty      e.g. 1,7,BUY,LIMIT,GTC,100,5   (MARKET price: 0 or empty)
//   cancel,id
//   amend,id,new_qty,new_price
// Blank lines and lines starting with '#' are ignored; a first line starting with "ts" is a
// header. Prints every event, then the final book. Exit code 2 on malformed input.

#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "finforge/matching_engine.hpp"

using namespace finforge;

namespace {

constexpr InstrumentId kInst{0};

std::vector<std::string_view> split(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = s.find(',', start);
        std::string_view f = s.substr(start, comma - start);
        while (!f.empty() && (f.front() == ' ' || f.front() == '\t')) f.remove_prefix(1);
        while (!f.empty() && (f.back() == ' ' || f.back() == '\t' || f.back() == '\r')) {
            f.remove_suffix(1);
        }
        out.push_back(f);
        if (comma == std::string_view::npos) return out;
        start = comma + 1;
    }
}

template <class T>
std::optional<T> parse_int(std::string_view s) {
    T v{};
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
    return v;
}

struct ParseError {
    std::string msg;
};

template <class T>
T need_int(std::string_view s, const char* what) {
    if (auto v = parse_int<T>(s)) return *v;
    throw ParseError{std::string("bad ") + what + " '" + std::string(s) + "'"};
}

template <class E>
E need_enum(std::string_view s, std::initializer_list<E> values, const char* what) {
    for (E e : values) {
        if (to_string(e) == s) return e;
    }
    throw ParseError{std::string("bad ") + what + " '" + std::string(s) + "'"};
}

void print_events(std::ostream& out, std::span<const BookEvent> events) {
    for (const auto& e : events) {
        if (const auto* u = std::get_if<OrderUpdate>(&e)) {
            out << "  " << to_string(u->status) << " id=" << u->order.value()
                << " filled=" << u->filled.value() << " remaining=" << u->remaining.value();
            if (u->reject_reason != RejectReason::None)
                out << " reason=" << to_string(u->reject_reason);
            if (u->cancel_reason != CancelReason::None)
                out << " reason=" << to_string(u->cancel_reason);
            out << '\n';
        } else if (const auto* t = std::get_if<Trade>(&e)) {
            out << "  TRADE #" << t->id.value() << " px=" << t->price.value()
                << " qty=" << t->qty.value() << " aggressor=" << t->aggressor.value() << " ("
                << to_string(t->aggressor_side) << ", owner " << t->aggressor_owner.value()
                << ") resting=" << t->resting.value() << " (owner " << t->resting_owner.value()
                << ")\n";
        } else if (const auto* a = std::get_if<OrderAmended>(&e)) {
            out << "  AMENDED id=" << a->order.value() << " qty=" << a->qty.value()
                << " remaining=" << a->remaining.value() << '\n';
        }
    }
}

void print_action(std::ostream& out, const ActionResult& r) {
    if (!r.ok()) out << "  REFUSED reason=" << to_string(r.reason) << '\n';
    if (r.new_order.value() != 0) out << "  replaced by id=" << r.new_order.value() << '\n';
}

void print_book(std::ostream& out, const OrderBook& book) {
    out << "FINAL BOOK\n  ASKS (best last)\n";
    auto asks = book.depth(Side::Sell, SIZE_MAX);
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
        out << "    " << it->price.value() << "  qty=" << it->qty.value()
            << "  orders=" << it->orders << '\n';
    }
    out << "  BIDS (best first)\n";
    for (const LevelInfo& l : book.depth(Side::Buy, SIZE_MAX)) {
        out << "    " << l.price.value() << "  qty=" << l.qty.value() << "  orders=" << l.orders
            << '\n';
    }
    if (const auto s = book.spread()) out << "  spread=" << s->value() << '\n';
}

int run(std::istream& in, std::ostream& out) {
    InstrumentRegistry reg;
    reg.add({.symbol = "SIM", .quote_currency = "USD", .tick_size = "1", .lot_size = "1"});
    MatchingEngine eng(std::move(reg));

    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        const auto f = split(line);
        if (f.size() == 1 && f[0].empty()) continue;
        if (!f[0].empty() && f[0].front() == '#') continue;
        if (lineno == 1 && f[0] == "ts") continue;
        try {
            if (f[0] == "cancel") {
                if (f.size() != 2) throw ParseError{"expected cancel,id"};
                const auto id = need_int<std::uint64_t>(f[1], "id");
                out << "[" << lineno << "] cancel id=" << id << '\n';
                const ActionResult r = eng.cancel(OrderId{id});
                print_events(out, eng.events());
                print_action(out, r);
                continue;
            } else if (f[0] == "amend") {
                if (f.size() != 4) throw ParseError{"expected amend,id,new_qty,new_price"};
                const auto id = need_int<std::uint64_t>(f[1], "id");
                const auto q = need_int<std::int64_t>(f[2], "qty");
                const auto p = need_int<std::int64_t>(f[3], "price");
                out << "[" << lineno << "] amend id=" << id << " qty=" << q << " px=" << p << '\n';
                const ActionResult r = eng.amend(OrderId{id}, Qty{q}, Price{p}, Timestamp{0});
                print_events(out, eng.events());
                print_action(out, r);
                continue;
            } else {
                if (f.size() != 7) throw ParseError{"expected ts,owner,side,type,tif,price,qty"};
                NewOrder o;
                o.ts = Timestamp{need_int<std::int64_t>(f[0], "ts")};
                o.owner = OwnerId{need_int<std::uint32_t>(f[1], "owner")};
                o.instrument = kInst;
                o.side = need_enum(f[2], {Side::Buy, Side::Sell}, "side");
                o.type = need_enum(f[3], {OrderType::Market, OrderType::Limit}, "type");
                o.tif = need_enum(f[4], {Tif::Gtc, Tif::Ioc, Tif::Fok}, "tif");
                o.price = Price{f[5].empty() ? 0 : need_int<std::int64_t>(f[5], "price")};
                o.qty = Qty{need_int<std::int64_t>(f[6], "qty")};
                const OrderId id = eng.submit(o);
                out << "[" << lineno << "] submit id=" << id.value() << ' ' << to_string(o.side)
                    << ' ' << to_string(o.type) << ' ' << to_string(o.tif)
                    << " px=" << o.price.value() << " qty=" << o.qty.value()
                    << " owner=" << o.owner.value() << '\n';
            }
            print_events(out, eng.events());
        } catch (const ParseError& e) {
            std::cerr << "finforge-sim: line " << lineno << ": " << e.msg << '\n';
            return 2;
        }
    }
    print_book(out, eng.book(kInst));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "usage: finforge-sim [orders.csv]   (reads stdin when no file is given)\n";
        return 2;
    }
    if (argc == 2) {
        std::ifstream f(argv[1]);
        if (!f) {
            std::cerr << "finforge-sim: cannot open " << argv[1] << '\n';
            return 2;
        }
        return run(f, std::cout);
    }
    return run(std::cin, std::cout);
}
