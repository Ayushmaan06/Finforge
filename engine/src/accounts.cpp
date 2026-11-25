#include "finforge/accounts.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "finforge/assert.hpp"

namespace finforge {

namespace {

constexpr std::int64_t kRateScale = 1'000'000;       // Rate unit = 1e-6
constexpr std::int64_t kDayNs = 86'400'000'000'000;  // UTC day (TIME-1: no local time)
constexpr std::int64_t kDaysPerYear = 365;           // PLAN D5: crypto trades every day

std::int64_t to_i64(int128 v, const char* what) {
    if (v > std::numeric_limits<std::int64_t>::max() ||
        v < std::numeric_limits<std::int64_t>::min()) {
        throw NumericOverflow(std::string("NUMERIC_OVERFLOW: ") + what);
    }
    return static_cast<std::int64_t>(v);
}

// ceil(a / b) for a >= 0, b > 0.
int128 ceil_div(int128 a, int128 b) { return (a + b - 1) / b; }

std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

Money abs(Money m) { return m < Money{0} ? Money{0} - m : m; }
Qty abs(Qty q) { return q < Qty{0} ? Qty{0} - q : q; }

}  // namespace

// --- Ledger -------------------------------------------------------------------------------------

Ledger::Ledger(const InstrumentRegistry& registry, AccountConfig config)
    : config_(config), cash_(config.initial_cash), positions_(registry.size()) {
    if (config_.initial_cash < Money{0}) throw std::invalid_argument("initial_cash < 0");
    if (config_.fees.maker < Rate{0} || config_.fees.taker < Rate{0} ||
        config_.fees.min_fee < Money{0} || config_.max_position < Qty{0} ||
        config_.max_leverage <= Rate{0} || config_.borrow_rate < Rate{0}) {
        throw std::invalid_argument("invalid account config");
    }
    units_.reserve(registry.size());
    for (std::size_t i = 0; i < registry.size(); ++i) {
        const Instrument* inst = registry.find(InstrumentId{static_cast<std::uint32_t>(i)});
        units_.push_back({inst->money_per_tick_lot, inst->spec.contract_multiplier});
    }
}

Money Ledger::notional(InstrumentId id, Price price, Qty qty) const {
    const Unit& u = units_[id.value()];
    return finforge::notional(price, qty, u.money_per_tick_lot, u.multiplier);  // NUM-3
}

Money Ledger::fee(Money n, Liquidity liquidity) const {
    const Rate rate = liquidity == Liquidity::Maker ? config_.fees.maker : config_.fees.taker;
    const int128 a = static_cast<int128>(abs(n).value()) * rate.value();
    const Money proportional{to_i64(ceil_div(a, kRateScale), "fee")};  // NUM-5: fees round up
    if (n == Money{0}) return Money{0};
    return std::max(proportional, config_.fees.min_fee);
}

void Ledger::apply(const LedgerFill& f) {
    FINFORGE_CHECK(f.qty > Qty{0} && f.price > Price{0}, "fill must have qty > 0 and price > 0");
    FINFORGE_CHECK(f.fee >= Money{0}, "negative fee");
    Position& p = positions_[f.instrument.value()];
    Qty rest = f.qty;
    const bool reducing =
        (f.side == Side::Buy && p.qty < Qty{0}) || (f.side == Side::Sell && p.qty > Qty{0});
    if (reducing) {
        const Qty close = std::min(rest, abs(p.qty));
        reduce(p, f.instrument, f.side, f.price, close);
        rest -= close;
    }
    if (rest > Qty{0}) increase(p, f.instrument, f.side, f.price, rest);  // ACC-4: the flip's open
    cash_ -= f.fee;                                                       // ACC-2
    fees_ += f.fee;
    entries_.emplace_back(f);
}

void Ledger::apply(const BorrowCharge& c) {
    FINFORGE_CHECK(c.amount >= Money{0}, "negative borrow charge");
    cash_ -= c.amount;
    borrow_ += c.amount;
    entries_.emplace_back(c);
}

// ACC-1, ACC-5: a buy pays, a sell (incl. a short sale) receives; the basis carries the sign.
void Ledger::increase(Position& p, InstrumentId id, Side side, Price price, Qty qty) {
    const Money n = notional(id, price, qty);
    const Money signed_n = side == Side::Buy ? n : Money{0} - n;
    p.cost_basis += signed_n;
    cash_ -= signed_n;
    p.qty += side == Side::Buy ? qty : Qty{0} - qty;
}

// ACC-3: release basis pro rata, truncated toward zero; all of it when the position closes.
void Ledger::reduce(Position& p, InstrumentId id, Side side, Price price, Qty qty) {
    const Money n = notional(id, price, qty);
    const Money proceeds = side == Side::Sell ? n : Money{0} - n;  // signed cash flow
    const Qty size = abs(p.qty);
    Money released = p.cost_basis;
    if (qty < size) {
        released = Money{to_i64(static_cast<int128>(p.cost_basis.value()) * qty.value() /
                                    size.value(),  // C++ division truncates toward zero
                                "basis release")};
    }
    realised_ += proceeds - released;
    p.cost_basis -= released;
    cash_ += proceeds;
    p.qty += side == Side::Buy ? qty : Qty{0} - qty;
    if (p.qty == Qty{0}) FINFORGE_CHECK(p.cost_basis == Money{0}, "basis left on a flat position");
}

Money Ledger::market_value(InstrumentId id, Price mark) const {
    return notional(id, mark, positions_[id.value()].qty);
}

Money Ledger::equity(std::span<const Price> marks) const {
    Money e = cash_;
    for (std::size_t i = 0; i < positions_.size(); ++i) {
        e += market_value(InstrumentId{static_cast<std::uint32_t>(i)}, marks[i]);
    }
    return e;
}

Money Ledger::unrealised_pnl(std::span<const Price> marks) const {
    Money u{0};
    for (std::size_t i = 0; i < positions_.size(); ++i) {
        u += market_value(InstrumentId{static_cast<std::uint32_t>(i)}, marks[i]) -
             positions_[i].cost_basis;
    }
    return u;
}

Money Ledger::gross_exposure(std::span<const Price> marks) const {
    Money g{0};
    for (std::size_t i = 0; i < positions_.size(); ++i) {
        g += abs(market_value(InstrumentId{static_cast<std::uint32_t>(i)}, marks[i]));
    }
    return g;
}

bool Ledger::reconciles() const {
    int128 lhs = cash_.value();
    for (const Position& p : positions_) lhs += p.cost_basis.value();
    const int128 rhs = static_cast<int128>(config_.initial_cash.value()) + realised_.value() -
                       fees_.value() - borrow_.value();
    return lhs == rhs;
}

bool Ledger::reconciles(std::span<const Price> marks) const {
    const int128 lhs = equity(marks).value();
    const int128 rhs = static_cast<int128>(config_.initial_cash.value()) + realised_.value() +
                       unrealised_pnl(marks).value() - fees_.value() - borrow_.value();
    return lhs == rhs;
}

Ledger Ledger::replayed() const {
    Ledger r = *this;
    r.cash_ = config_.initial_cash;
    r.reserved_ = Money{0};
    r.realised_ = r.fees_ = r.borrow_ = Money{0};
    std::fill(r.positions_.begin(), r.positions_.end(), Position{});
    r.entries_.clear();
    for (const LedgerEntry& e : entries_) std::visit([&](const auto& x) { r.apply(x); }, e);
    return r;
}

bool Ledger::same_state(const Ledger& o) const {
    return cash_ == o.cash_ && realised_ == o.realised_ && fees_ == o.fees_ &&
           borrow_ == o.borrow_ && positions_ == o.positions_;
}

// --- Accounts -----------------------------------------------------------------------------------

Accounts::Accounts(ExecutionModel& model, AccountsConfig config)
    : model_(model), config_(config), last_fill_(model.instrument_count()) {
    if (config_.equity_interval_ns < 0) throw std::invalid_argument("equity_interval_ns < 0");
    model_.set_account_hooks(this);
}

Accounts::~Accounts() {
    if (model_.account_hooks() == this) model_.set_account_hooks(nullptr);
}

Ledger& Accounts::open(OwnerId owner, AccountConfig config) {
    if (accounts_.size() <= owner.value()) accounts_.resize(owner.value() + 1);
    if (accounts_[owner.value()]) throw std::logic_error("account already open");
    accounts_[owner.value()] = std::make_unique<Account>(model_.registry(), config);
    return accounts_[owner.value()]->ledger;
}

Accounts::Account* Accounts::find(OwnerId owner) {
    return owner.value() < accounts_.size() ? accounts_[owner.value()].get() : nullptr;
}
const Accounts::Account* Accounts::find(OwnerId owner) const {
    return owner.value() < accounts_.size() ? accounts_[owner.value()].get() : nullptr;
}

const Ledger* Accounts::ledger(OwnerId owner) const {
    const Account* a = find(owner);
    return a ? &a->ledger : nullptr;
}

Price Accounts::mark(InstrumentId id) const {
    if (const auto m = model_.mark(id)) return *m;
    if (id.value() < last_fill_.size() && last_fill_[id.value()]) return *last_fill_[id.value()];
    return Price{0};
}

std::vector<Price> Accounts::marks() const {
    std::vector<Price> m(model_.instrument_count());
    for (std::size_t i = 0; i < m.size(); ++i)
        m[i] = mark(InstrumentId{static_cast<std::uint32_t>(i)});
    return m;
}

Qty Accounts::projected(const Account& a, InstrumentId id) const {
    Qty q = a.ledger.position(id).qty;
    for (const auto& [oid, o] : a.open) {
        if (o.instrument == id) q += o.side == Side::Buy ? o.remaining : Qty{0} - o.remaining;
    }
    return q;
}

RejectReason Accounts::pre_trade(const NewOrder& o, Price reference) {
    Account* a = find(o.owner);
    if (a == nullptr) return RejectReason::None;
    const Ledger& l = a->ledger;
    const AccountConfig& c = l.config();
    const Money n = l.notional(o.instrument, reference, o.qty);
    const Money fee = l.fee(n, Liquidity::Taker);

    // ACC-9, in order; the first failure is the reason.
    if (o.side == Side::Buy && l.cash() - l.reserved_cash() < n + fee) {
        return RejectReason::InsufficientCash;
    }
    const Qty signed_qty = o.side == Side::Buy ? o.qty : Qty{0} - o.qty;
    const Qty after = projected(*a, o.instrument) + signed_qty;
    if (c.max_position > Qty{0} && abs(after) > c.max_position) return RejectReason::PositionLimit;
    if (!c.allow_short && after < Qty{0}) return RejectReason::ShortNotAllowed;

    // Gross leverage after the order: Σ |projected position × mark| / (equity − fee).
    std::vector<Price> m = marks();
    if (m[o.instrument.value()] == Price{0}) m[o.instrument.value()] = reference;
    int128 gross = 0;
    for (std::size_t i = 0; i < m.size(); ++i) {
        const InstrumentId id{static_cast<std::uint32_t>(i)};
        const Qty q = id == o.instrument ? after : projected(*a, id);
        gross += abs(l.notional(id, m[i], q)).value();
    }
    const int128 equity = static_cast<int128>(l.equity(m).value()) - fee.value();
    if (gross > 0 && (equity <= 0 || gross * kRateScale > equity * c.max_leverage.value())) {
        return RejectReason::LeverageLimit;
    }
    return RejectReason::None;
}

Qty Accounts::affordable(const Order& o, Price price, Qty wanted) {
    Account* a = find(o.owner);
    if (a == nullptr || o.side != Side::Buy) return wanted;
    const Ledger& l = a->ledger;
    Money own{0};
    if (auto it = a->open.find(o.id); it != a->open.end()) own = it->second.reserved;
    // Cash not promised to the account's other open orders.
    const Money avail = l.cash() - (l.reserved_cash() - own);
    auto cost = [&](Qty q) {
        const Money n = l.notional(o.instrument, price, q);
        return n + l.fee(n, Liquidity::Taker);
    };
    if (cost(wanted) <= avail) return wanted;
    // Largest q in [0, wanted] with cost(q) <= avail; cost is non-decreasing in q.
    std::int64_t lo = 0;
    std::int64_t hi = wanted.value();
    while (lo < hi) {
        const std::int64_t mid = lo + (hi - lo + 1) / 2;
        if (cost(Qty{mid}) <= avail) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return Qty{lo};
}

void Accounts::on_fill(const Order& o, const Trade& t, Liquidity liquidity) {
    last_fill_[t.instrument.value()] = t.price;
    Account* a = find(o.owner);
    if (a == nullptr) return;
    Ledger& l = a->ledger;
    const Money fee = l.fee(l.notional(t.instrument, t.price, t.qty), liquidity);
    l.apply(LedgerFill{t.ts, o.id, t.id, t.instrument, o.side, t.price, t.qty, fee, liquidity});
#ifdef FINFORGE_EXPENSIVE_CHECKS
    FINFORGE_CHECK(l.reconciles() && l.reconciles(marks()), "ledger does not reconcile (ACC-10)");
#endif
}

void Accounts::on_order_update(const Order& o) {
    Account* a = find(o.owner);
    if (a == nullptr) return;
    Money reserved_total = a->ledger.reserved_cash();
    if (auto it = a->open.find(o.id); it != a->open.end()) {
        reserved_total -= it->second.reserved;
        a->open.erase(it);
    }
    if (o.status == OrderStatus::Accepted || o.status == OrderStatus::PartiallyFilled) {
        Money reserved{0};
        if (o.side == Side::Buy && o.type == OrderType::Limit) {  // ACC-7
            const Money n = a->ledger.notional(o.instrument, o.price, o.remaining);
            reserved = n + a->ledger.fee(n, Liquidity::Taker);
        }
        a->open.emplace(o.id, OpenOrder{o.instrument, o.side, o.remaining, reserved});
        reserved_total += reserved;
    }
    a->ledger.set_reserved_cash(reserved_total);
}

void Accounts::accrue_borrow(Timestamp midnight) {
    const std::vector<Price> m = marks();
    for (auto& a : accounts_) {
        if (!a || a->ledger.config().borrow_rate == Rate{0}) continue;
        int128 short_value = 0;
        for (std::size_t i = 0; i < m.size(); ++i) {
            const InstrumentId id{static_cast<std::uint32_t>(i)};
            const Money mv = a->ledger.market_value(id, m[i]);
            if (mv < Money{0}) short_value -= mv.value();
        }
        // ACC-8: ceil(|short market value| × annual rate / 1e6 / 365).
        const int128 cost = ceil_div(short_value * a->ledger.config().borrow_rate.value(),
                                     static_cast<int128>(kRateScale) * kDaysPerYear);
        if (cost > 0) a->ledger.apply(BorrowCharge{midnight, Money{to_i64(cost, "borrow")}});
    }
}

void Accounts::advance(Timestamp now) {
    if (!last_advance_) {
        last_advance_ = now;
        if (config_.equity_interval_ns > 0) {
            next_interval_ = (floor_div(now.value(), config_.equity_interval_ns) + 1) *
                             config_.equity_interval_ns;
        }
        return;
    }
    if (now <= *last_advance_) return;
    // Every UTC midnight in (last, now], in order (ACC-8).
    for (std::int64_t day = floor_div(last_advance_->value(), kDayNs) + 1;
         day * kDayNs <= now.value(); ++day) {
        accrue_borrow(Timestamp{day * kDayNs});
    }
    if (config_.equity_interval_ns > 0) {
        while (next_interval_ <= now.value()) {
            snapshot(Timestamp{next_interval_});
            next_interval_ += config_.equity_interval_ns;
        }
    }
    last_advance_ = now;
}

void Accounts::after_market_data(const MarketData& md, Timestamp now) {
    if (config_.equity_interval_ns == 0 && std::holds_alternative<Bar>(md)) snapshot(now);
}

void Accounts::snapshot(Timestamp ts) {
    const std::vector<Price> m = marks();
    for (auto& a : accounts_) {
        if (!a) continue;
        const Ledger& l = a->ledger;
        const EquityPoint p{ts,
                            l.cash(),
                            l.equity(m),
                            l.realised_pnl(),
                            l.unrealised_pnl(m),
                            l.total_fees(),
                            l.borrow_costs(),
                            l.gross_exposure(m)};
        if (!a->equity.empty() && a->equity.back().ts == ts) {
            a->equity.back() = p;  // one point per timestamp (several instruments' bars)
        } else {
            a->equity.push_back(p);
        }
    }
}

void Accounts::finish(Timestamp now) {
    snapshot(now);
    for (std::size_t i = 0; i < accounts_.size(); ++i) {
        if (!accounts_[i]) continue;
        FINFORGE_CHECK(accounts_[i]->open.empty(), "open orders left at the end of the run (OP-3)");
        FINFORGE_CHECK(reconciles(OwnerId{static_cast<std::uint32_t>(i)}),
                       "account does not reconcile at the end of the run (ACC-10/11)");
    }
}

std::span<const EquityPoint> Accounts::equity_curve(OwnerId owner) const {
    const Account* a = find(owner);
    return a ? std::span<const EquityPoint>(a->equity) : std::span<const EquityPoint>{};
}

bool Accounts::reconciles(OwnerId owner) const {
    const Account* a = find(owner);
    if (a == nullptr) return true;
    const Ledger& l = a->ledger;
    return l.reconciles() && l.reconciles(marks()) && l.replayed().same_state(l);
}

}  // namespace finforge
