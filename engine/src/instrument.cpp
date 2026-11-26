#include "finforge/instrument.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace finforge {
namespace {

// An exact non-negative decimal: value = mantissa × 10^-scale.
struct Decimal {
    int128 mantissa = 0;
    int scale = 0;
};

constexpr int kMaxDigits = 18;

Decimal parse_decimal(std::string_view s, std::string_view field) {
    auto fail = [&](const char* why) {
        throw InvalidInstrument(std::string(field) + " '" + std::string(s) + "': " + why);
    };
    if (s.empty()) fail("empty");
    Decimal d;
    bool seen_point = false;
    int digits = 0;
    for (char c : s) {
        if (c == '.') {
            if (seen_point) fail("more than one decimal point");
            seen_point = true;
            continue;
        }
        if (c < '0' || c > '9') fail("not a plain decimal (digits and one '.')");
        if (d.mantissa != 0 || c != '0') ++digits;  // leading zeros are not significant
        if (digits > kMaxDigits) fail("too many significant digits");
        d.mantissa = d.mantissa * 10 + (c - '0');
        if (seen_point) ++d.scale;
    }
    if (s.front() == '.' || s.back() == '.') fail("decimal point needs digits on both sides");
    // Leading zeros after the point are not significant digits but still raise the scale; an
    // unbounded scale would overflow pow10() below (int128, UB).
    if (d.scale > kMaxDigits) fail("more than 18 decimal places");
    return d;
}

int128 pow10(int n) {
    int128 r = 1;
    for (int i = 0; i < n; ++i) r *= 10;
    return r;
}

}  // namespace

std::int64_t compute_money_per_tick_lot(std::string_view tick_size, std::string_view lot_size) {
    const Decimal tick = parse_decimal(tick_size, "tick_size");
    const Decimal lot = parse_decimal(lot_size, "lot_size");
    if (tick.mantissa == 0 || lot.mantissa == 0) {
        throw InvalidInstrument("tick_size and lot_size must be > 0");
    }
    // tick × lot / 1e-8 = (mt × ml) × 10^(8 - st - sl). Mantissas have <= 18 digits, so the
    // product is < 10^36 and fits in int128 (max ~1.7e38).
    int128 m = tick.mantissa * lot.mantissa;
    const int exp = 8 - tick.scale - lot.scale;
    if (exp >= 0) {
        if (__builtin_mul_overflow(m, pow10(exp), &m)) {
            throw InvalidInstrument("money_per_tick_lot does not fit int64");
        }
    } else {
        const int128 div = pow10(-exp);  // scales <= 18 each, so -exp <= 28 < 38
        if (m % div != 0) {
            throw InvalidInstrument(
                "money_per_tick_lot = tick_size × lot_size / 1e-8 is not an "
                "integer (NUM-2)");
        }
        m /= div;
    }
    if (m > std::numeric_limits<std::int64_t>::max()) {
        throw InvalidInstrument("money_per_tick_lot does not fit int64");
    }
    return static_cast<std::int64_t>(m);
}

InstrumentId InstrumentRegistry::add(InstrumentSpec spec) {
    if (spec.symbol.empty()) throw InvalidInstrument("symbol is empty");
    if (spec.min_qty < Qty{1}) throw InvalidInstrument(spec.symbol + ": min_qty must be >= 1 lot");
    if (spec.contract_multiplier < 1) {
        throw InvalidInstrument(spec.symbol + ": contract_multiplier must be >= 1");
    }
    const std::int64_t mptl = compute_money_per_tick_lot(spec.tick_size, spec.lot_size);
    const InstrumentId id{static_cast<InstrumentId::rep>(instruments_.size())};
    instruments_.push_back(Instrument{id, std::move(spec), mptl});
    return id;
}

const Instrument* InstrumentRegistry::find(InstrumentId id) const {
    return id.value() < instruments_.size() ? &instruments_[id.value()] : nullptr;
}

}  // namespace finforge
