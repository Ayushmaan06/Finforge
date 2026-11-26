#include "finforge/instrument.hpp"

#include <gtest/gtest.h>

using namespace finforge;

// NUM-2: examples from contracts.md.
TEST(Instrument, NUM2_MoneyPerTickLotExamples) {
    EXPECT_EQ(compute_money_per_tick_lot("0.01", "0.00001"), 10);   // BTCUSDT spot
    EXPECT_EQ(compute_money_per_tick_lot("0.01", "0.0001"), 100);   // ETHUSDT spot
    EXPECT_EQ(compute_money_per_tick_lot("0.1", "0.001"), 10'000);  // BTCUSDT USD-M
    EXPECT_EQ(compute_money_per_tick_lot("1", "1"), 100'000'000);
    EXPECT_EQ(compute_money_per_tick_lot("0.0001", "0.0001"), 1);      // smallest allowed
    EXPECT_EQ(compute_money_per_tick_lot("0.010", "0.00001000"), 10);  // trailing zeros
    EXPECT_EQ(compute_money_per_tick_lot("00.5", "2"), 100'000'000);   // leading zeros
}

TEST(Instrument, NUM2_RejectsNonIntegerOrTooSmall) {
    EXPECT_THROW(compute_money_per_tick_lot("0.00001", "0.0001"), InvalidInstrument);  // 0.1
    EXPECT_THROW(compute_money_per_tick_lot("0.03", "0.0000001"), InvalidInstrument);  // 0.3
    EXPECT_THROW(compute_money_per_tick_lot("0.000000001", "1"), InvalidInstrument);
}

TEST(Instrument, NUM2_RejectsOutOfRange) {
    EXPECT_THROW(compute_money_per_tick_lot("999999999999999999", "999999999999999999"),
                 InvalidInstrument);
    // Leading zeros raise the scale without counting as significant digits: 19 and 60 decimal
    // places must be refused, not overflow pow10 (int128 UB before Phase 9).
    EXPECT_THROW(compute_money_per_tick_lot("0.0000000000000000001", "1"), InvalidInstrument);
    EXPECT_THROW(compute_money_per_tick_lot(
                     "0.000000000000000000000000000000000000000000000000000000000001", "1"),
                 InvalidInstrument);
    // Exactly 18 places is still fine: 1e-18 × 1e10 = 1e-8 = one Money unit.
    EXPECT_EQ(compute_money_per_tick_lot("0.000000000000000001", "10000000000"), 1);
}

TEST(Instrument, INS1_RejectsMalformedDecimals) {
    for (const char* bad : {"", "0", "0.0", "-0.01", "1e-2", "0.01.0", ".5", "5.", " 1", "abc",
                            "1234567890123456789"}) {
        EXPECT_THROW(compute_money_per_tick_lot(bad, "1"), InvalidInstrument) << bad;
        EXPECT_THROW(compute_money_per_tick_lot("1", bad), InvalidInstrument) << bad;
    }
}

TEST(Instrument, INS1_RegistryAssignsDenseIds) {
    InstrumentRegistry reg;
    EXPECT_EQ(reg.add({.symbol = "BTCUSDT",
                       .quote_currency = "USDT",
                       .tick_size = "0.01",
                       .lot_size = "0.00001"}),
              InstrumentId{0});
    EXPECT_EQ(reg.add({.symbol = "ETHUSDT",
                       .quote_currency = "USDT",
                       .tick_size = "0.01",
                       .lot_size = "0.0001"}),
              InstrumentId{1});
    ASSERT_NE(reg.find(InstrumentId{1}), nullptr);
    EXPECT_EQ(reg.find(InstrumentId{1})->spec.symbol, "ETHUSDT");
    EXPECT_EQ(reg.find(InstrumentId{1})->money_per_tick_lot, 100);
    EXPECT_EQ(reg.find(InstrumentId{2}), nullptr);
}

TEST(Instrument, INS1_RegistryRejectsBadSpecs) {
    InstrumentRegistry reg;
    EXPECT_THROW(reg.add({.symbol = "", .tick_size = "1", .lot_size = "1"}), InvalidInstrument);
    EXPECT_THROW(reg.add({.symbol = "X", .tick_size = "1", .lot_size = "1", .min_qty = Qty{0}}),
                 InvalidInstrument);
    EXPECT_THROW(
        reg.add({.symbol = "X", .tick_size = "1", .lot_size = "1", .contract_multiplier = 0}),
        InvalidInstrument);
    EXPECT_EQ(reg.size(), 0u);
}
