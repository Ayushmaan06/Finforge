#pragma once

// Instruments and the registry (contracts.md §3, NUM-2).

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "finforge/types.hpp"

namespace finforge {

class InvalidInstrument : public std::invalid_argument {
  public:
    explicit InvalidInstrument(const std::string& what) : std::invalid_argument(what) {}
};

// What a dataset manifest declares. tick_size and lot_size are decimal strings, never doubles
// (INS-1).
struct InstrumentSpec {
    std::string symbol{};
    std::string asset_class = "spot";
    std::string quote_currency{};
    std::string tick_size{};
    std::string lot_size{};
    Qty min_qty{1};
    std::int64_t contract_multiplier = 1;
};

struct Instrument {
    InstrumentId id;
    InstrumentSpec spec;
    std::int64_t money_per_tick_lot = 0;  // NUM-2: exact integer >= 1
};

// NUM-2: tick_size × lot_size / 1e-8, which must be an exact integer >= 1.
// Throws InvalidInstrument for malformed decimals or a non-integer / out-of-range result.
std::int64_t compute_money_per_tick_lot(std::string_view tick_size, std::string_view lot_size);

class InstrumentRegistry {
  public:
    // Validates the spec and assigns the next dense id (INS-1). Throws InvalidInstrument.
    InstrumentId add(InstrumentSpec spec);

    // nullptr for an unknown id.
    [[nodiscard]] const Instrument* find(InstrumentId id) const;
    [[nodiscard]] std::size_t size() const { return instruments_.size(); }

  private:
    std::vector<Instrument> instruments_;
};

}  // namespace finforge
