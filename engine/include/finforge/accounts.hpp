#pragma once

// Portfolio accounting and pre-trade checks (contracts.md §8 FEE-1, §9 ACC-1…11).
//
// Ledger: one account's cash, positions and cost bases, exact in integer Money. It stores the cost
// basis, not an average price, so ACC-10 is an exact identity:
//     cash + Σ cost_basis == initial_cash + realised_pnl − total_fees − borrow_costs
// and, with marks, equity == initial_cash + realised + unrealised − fees − borrow.
//
// Accounts: the AccountHooks an execution model reports to. Owns one Ledger per owner that has an
// account, runs the ACC-9 checks, caps buys at fill time (OP-1), tracks reservations (ACC-7),
// accrues borrow (ACC-8), records equity snapshots and checks reconciliation.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "finforge/events.hpp"
#include "finforge/execution_model.hpp"
#include "finforge/instrument.hpp"
#include "finforge/order.hpp"
#include "finforge/types.hpp"

namespace finforge {

struct FeeSchedule {
    Rate maker{0};  // 10 bps = Rate{1000}
    Rate taker{0};
    Money min_fee{0};  // per fill
};

struct AccountConfig {
    Money initial_cash{0};
    FeeSchedule fees{};
    bool allow_short = false;
    Qty max_position{0};           // per instrument, |projected position| in lots; 0 = no limit
    Rate max_leverage{1'000'000};  // gross exposure / equity; 1.0
    Rate borrow_rate{0};           // annual, on short market value (ACC-8)
};

struct Position {
    Qty qty;           // signed lots
    Money cost_basis;  // signed with the position

    friend bool operator==(const Position&, const Position&) = default;
};

// One fill as the ledger saw it (the fill ledger replayed by ACC-11).
struct LedgerFill {
    Timestamp ts;
    OrderId order;
    TradeId trade;
    InstrumentId instrument;
    Side side = Side::Buy;
    Price price;
    Qty qty;
    Money fee;
    Liquidity liquidity = Liquidity::Taker;
};

struct BorrowCharge {
    Timestamp ts;
    Money amount;
};

using LedgerEntry = std::variant<LedgerFill, BorrowCharge>;

class Ledger {
  public:
    Ledger(const InstrumentRegistry& registry, AccountConfig config);

    [[nodiscard]] const AccountConfig& config() const { return config_; }

    // NUM-3 notional of `qty` lots at `price` (signed like qty).
    [[nodiscard]] Money notional(InstrumentId id, Price price, Qty qty) const;
    // FEE-1: max(ceil(notional × rate / 1e6), min_fee); 0 for a zero notional.
    [[nodiscard]] Money fee(Money notional, Liquidity liquidity) const;

    // ACC-1…5: one fill, fee included (a flip is a close then an open at the same price, ACC-4).
    void apply(const LedgerFill& fill);
    // ACC-8: borrow cost debited from cash.
    void apply(const BorrowCharge& charge);

    [[nodiscard]] Money cash() const { return cash_; }
    [[nodiscard]] Money reserved_cash() const { return reserved_; }
    void set_reserved_cash(Money m) { reserved_ = m; }
    [[nodiscard]] Money realised_pnl() const { return realised_; }
    [[nodiscard]] Money total_fees() const { return fees_; }
    [[nodiscard]] Money borrow_costs() const { return borrow_; }
    [[nodiscard]] const Position& position(InstrumentId id) const { return positions_[id.value()]; }
    [[nodiscard]] std::size_t instrument_count() const { return positions_.size(); }

    // ACC-6, `marks` indexed by instrument id.
    [[nodiscard]] Money market_value(InstrumentId id, Price mark) const;
    [[nodiscard]] Money equity(std::span<const Price> marks) const;
    [[nodiscard]] Money unrealised_pnl(std::span<const Price> marks) const;
    [[nodiscard]] Money gross_exposure(std::span<const Price> marks) const;

    // ACC-10 in its mark-free form, and with marks.
    [[nodiscard]] bool reconciles() const;
    [[nodiscard]] bool reconciles(std::span<const Price> marks) const;

    [[nodiscard]] std::span<const LedgerEntry> entries() const { return entries_; }
    // ACC-11: a fresh ledger with the same config, built by applying entries() in order.
    [[nodiscard]] Ledger replayed() const;
    // Cash, positions, bases, realised P&L, fees and borrow costs all equal.
    [[nodiscard]] bool same_state(const Ledger& other) const;

  private:
    struct Unit {
        std::int64_t money_per_tick_lot;
        std::int64_t multiplier;
    };
    void reduce(Position& p, InstrumentId id, Side side, Price price, Qty qty);
    void increase(Position& p, InstrumentId id, Side side, Price price, Qty qty);

    std::vector<Unit> units_;
    AccountConfig config_;
    Money cash_;
    Money reserved_;
    Money realised_;
    Money fees_;
    Money borrow_;
    std::vector<Position> positions_;
    std::vector<LedgerEntry> entries_;
};

struct EquityPoint {
    Timestamp ts;
    Money cash;
    Money equity;
    Money realised_pnl;
    Money unrealised_pnl;
    Money total_fees;
    Money borrow_costs;
    Money gross_exposure;
};

struct AccountsConfig {
    // Equity snapshot every `equity_interval_ns`; 0 = once per timestamp at which a bar arrives.
    std::int64_t equity_interval_ns = 0;
};

class Accounts final : public AccountHooks {
  public:
    // Registers itself as `model`'s account hooks; `model` must outlive it.
    explicit Accounts(ExecutionModel& model, AccountsConfig config = {});
    ~Accounts() override;
    Accounts(const Accounts&) = delete;
    Accounts& operator=(const Accounts&) = delete;

    // Opens the account of `owner` (before it trades). Throws std::logic_error if already open.
    Ledger& open(OwnerId owner, AccountConfig config);
    // nullptr for an owner without an account.
    [[nodiscard]] const Ledger* ledger(OwnerId owner) const;

    // --- AccountHooks ---
    RejectReason pre_trade(const NewOrder& order, Price reference) override;
    Qty affordable(const Order& order, Price price, Qty wanted) override;
    void on_fill(const Order& order, const Trade& trade, Liquidity liquidity) override;
    void on_order_update(const Order& order) override;

    // --- Driven by the simulator ---
    // Before each event: borrow at every UTC midnight crossed (ACC-8), interval snapshots.
    void advance(Timestamp now);
    // After a market data event was processed: per-bar equity snapshot.
    void after_market_data(const MarketData& md, Timestamp now);
    // End of run (after open orders were cancelled, OP-3): final snapshot, then ACC-10 and ACC-11
    // for every account; a failure trips FINFORGE_CHECK.
    void finish(Timestamp now);

    // ACC-6: the model's mark, else the last fill price, else 0.
    [[nodiscard]] Price mark(InstrumentId id) const;
    [[nodiscard]] std::vector<Price> marks() const;
    [[nodiscard]] std::span<const EquityPoint> equity_curve(OwnerId owner) const;
    // ACC-10 (both forms) and ACC-11 for one account.
    [[nodiscard]] bool reconciles(OwnerId owner) const;

  private:
    struct OpenOrder {
        InstrumentId instrument;
        Side side = Side::Buy;
        Qty remaining;
        Money reserved;
    };
    struct Account {
        Account(const InstrumentRegistry& r, AccountConfig c) : ledger(r, c) {}
        Ledger ledger;
        std::map<OrderId, OpenOrder> open;
        std::vector<EquityPoint> equity;
    };

    Account* find(OwnerId owner);
    const Account* find(OwnerId owner) const;
    Qty projected(const Account& a, InstrumentId id) const;  // position + open orders, signed
    void snapshot(Timestamp ts);
    void accrue_borrow(Timestamp midnight);

    ExecutionModel& model_;
    AccountsConfig config_;
    std::vector<std::unique_ptr<Account>> accounts_;  // index = owner id
    std::vector<std::optional<Price>> last_fill_;
    std::optional<Timestamp> last_advance_;
    std::int64_t next_interval_ = 0;
};

}  // namespace finforge
