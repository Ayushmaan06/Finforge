#!/usr/bin/env python3
"""Regenerate the committed test fixtures in datasets/fixtures/ (PLAN Phase 3 task 6).

The data is synthetic (no Binance data is redistributed) but uses Binance's file layouts. Each
malformed fixture contains a known set of problems; python/tests/test_data_fixtures.py lists the
findings each one must produce. Run from anywhere: python scripts/make_fixtures.py
"""

import json
import random
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "datasets" / "fixtures"
MINUTE_MS = 60_000
T0_MS = 1_709_251_200_000  # 2024-03-01T00:00:00Z
T0_US_2025 = 1_735_689_600_000_000  # 2025-01-01T00:00:00Z (Binance spot switched to µs)

KLINE_HEADER = (
    "open_time,open,high,low,close,volume,close_time,quote_volume,count,"
    "taker_buy_volume,taker_buy_quote_volume,ignore"
)
BOOK_HEADER = (
    "update_id,best_bid_price,best_bid_qty,best_ask_price,best_ask_qty,transaction_time,event_time"
)


def fmt(units: int, digits: int) -> str:
    """Integer units of 10^-digits as a Binance-style 8-decimal string."""
    s = f"{units:0{digits + 1}d}"
    whole, frac = s[:-digits] if digits else s, s[-digits:] if digits else ""
    return f"{whole}.{frac.ljust(8, '0')}"


def klines(
    n, start, step, tick_digits, tick_units, lot_digits, lot_units, price0, seed, unit_per_min
):
    """Rows of a clean random-walk kline file. Prices in ticks, volume in lots."""
    rng = random.Random(seed)
    rows = []
    price = price0
    for i in range(n):
        o = price
        c = max(10, o + rng.randint(-5, 5))
        h = max(o, c) + rng.randint(0, 3)
        lo = max(1, min(o, c) - rng.randint(0, 3))
        vol = rng.randint(0, 5000)
        t = start + i * step
        rows.append(
            [
                str(t),
                fmt(o * tick_units, tick_digits),
                fmt(h * tick_units, tick_digits),
                fmt(lo * tick_units, tick_digits),
                fmt(c * tick_units, tick_digits),
                fmt(vol * lot_units, lot_digits),
                str(t + unit_per_min - 1),
                "0.00000000",
                str(rng.randint(1, 100)),
                "0.00000000",
                "0.00000000",
                "0",
            ]
        )
        price = c
    return rows


def aaa_klines(n, start=T0_MS, seed=1, unit_per_min=MINUTE_MS):  # tick 0.01, lot 0.001
    return klines(n, start, unit_per_min, 2, 1, 3, 1, 10_000, seed, unit_per_min)


def bbb_klines(n, start=T0_MS, seed=2):  # tick 0.5 = 5 units at 1 digit, lot 1
    return klines(n, start, MINUTE_MS, 1, 5, 0, 1, 5_000, seed, MINUTE_MS)


def write_csv(name, rows, header=None):
    lines = ([header] if header else []) + [",".join(r) for r in rows]
    (OUT / name).write_text("\n".join(lines) + "\n")


def book(n, seed, start=T0_MS):
    rng = random.Random(seed)
    rows = []
    t = start
    bid = 10_000  # ticks of 0.01
    for i in range(n):
        t += rng.choice([0, 0, 1, 3, 10, 250])  # equal timestamps happen
        bid = max(100, bid + rng.randint(-2, 2))
        ask = bid + rng.randint(1, 3)
        rows.append(
            [
                str(1000 + i),
                fmt(bid, 2),
                fmt(rng.randint(1, 900), 3),
                fmt(ask, 2),
                fmt(rng.randint(1, 900), 3),
                str(t),
                str(t + rng.randint(0, 5)),
            ]
        )
    return rows


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    registry = {
        "instruments": [
            {
                "symbol": "AAA",
                "asset_class": "spot",
                "quote_currency": "USDT",
                "tick_size": "0.01",
                "lot_size": "0.001",
                "min_qty": 1,
                "contract_multiplier": 1,
            },
            {
                "symbol": "BBB",
                "asset_class": "spot",
                "quote_currency": "USDT",
                "tick_size": "0.5",
                "lot_size": "1",
                "min_qty": 1,
                "contract_multiplier": 1,
            },
        ]
    }
    (OUT / "registry.json").write_text(json.dumps(registry, indent=2) + "\n")

    # Clean files.
    write_csv("klines_AAA_ms.csv", aaa_klines(300))  # spot layout: no header
    write_csv("klines_BBB_ms.csv", bbb_klines(300), KLINE_HEADER)  # futures layout: header
    write_csv("klines_AAA_us.csv", aaa_klines(120, T0_US_2025, 3, MINUTE_MS * 1000))  # 2025: µs
    write_csv("bookticker_AAA.csv", book(300, 4), BOOK_HEADER)

    # TS_UNIT_AMBIGUOUS: 30 rows in ms, then 30 in µs (two files concatenated).
    ms = aaa_klines(30, seed=5)
    us = aaa_klines(30, (T0_MS + 30 * MINUTE_MS) * 1000, 5, MINUTE_MS * 1000)
    write_csv("klines_ambiguous.csv", ms + us)

    # OHLC_INCONSISTENT x2: row 5 high < close, row 12 low > open.
    rows = aaa_klines(40, seed=6)
    rows[4][2] = rows[4][4]  # high = close ...
    rows[4][4] = fmt(int(rows[4][4].replace(".", "")[:-6]) + 1, 2)  # ... close = high + 1 tick
    rows[11][3] = fmt(int(rows[11][1].replace(".", "")[:-6]) + 1, 2)  # low = open + 1 tick
    rows[11][2] = fmt(int(rows[11][3].replace(".", "")[:-6]) + 5, 2)  # keep low <= high
    write_csv("klines_ohlc.csv", rows)

    # DUPLICATE x1 (row 10 repeats row 9's open time), NON_MONOTONIC_TS x1 (row 21 has row 5's).
    rows = aaa_klines(39, seed=7)
    rows.insert(9, list(rows[8]))
    rows.insert(20, list(rows[4]))
    write_csv("klines_sequence.csv", rows)

    # GAP x1, 3 bars missing after row 15.
    rows = aaa_klines(43, seed=8)
    del rows[15:18]
    write_csv("klines_gap.csv", rows)

    # OFF_TICK x1 (row 3 open has a third decimal), OFF_LOT x1 (row 7 volume has a fourth).
    rows = aaa_klines(30, seed=9)
    rows[2][1] = rows[2][1][:-6] + "500000"  # e.g. 100.00 -> 100.005
    rows[6][5] = rows[6][5][:-4] + "5000"  # e.g. 1.000 -> 1.0005
    write_csv("klines_grid.csv", rows)

    # One of each value problem; the malformed row is last so row numbers stay simple.
    rows = aaa_klines(30, seed=10)
    rows[1][4] = ""  # row 2: MISSING_VALUE (close)
    rows[3][2] = "abc"  # row 4: BAD_NUMBER (high)
    rows[5][5] = "-" + rows[5][5]  # row 6: NEGATIVE_QTY
    for c in (1, 2, 3, 4):  # row 8: NON_POSITIVE_PRICE (all zero, so OHLC stays consistent)
        rows[7][c] = "0.00000000"
    rows[9][6] = str(int(rows[9][6]) + 1)  # row 10: BAD_CLOSE_TIME
    rows.append(rows[29][:5])  # MALFORMED_ROW (5 columns)
    write_csv("klines_values.csv", rows)

    # Quotes: CROSSED_QUOTE x2, DUPLICATE x1, NON_MONOTONIC_TS x1.
    rows = book(40, 11)
    for i in (5, 17):  # rows 6 and 18: bid above ask
        rows[i][1], rows[i][3] = rows[i][3], rows[i][1]
    rows.insert(25, [str(5000), *rows[24][1:]])  # row 26: same quote again (new update id)
    rows[33][5] = str(int(rows[2][5]) - 1)  # row 34: earlier than row 3
    write_csv("bookticker_dirty.csv", rows, BOOK_HEADER)

    # Generic trades CSV for BBB (tick 0.5, lot 1): side buy/sell, one unknown, one bad (row 20).
    rng = random.Random(12)
    t = T0_MS
    lines = ["time,price,size,side"]
    for i in range(50):
        t += rng.randint(0, 2000)
        side = rng.choice(["buy", "sell", "BUY", "Sell"])
        if i == 9:
            side = ""
        if i == 19:
            side = "x"
        lines.append(f"{t},{2500 + rng.randint(-20, 20) * 0.5:.1f},{rng.randint(1, 9)},{side}")
    (OUT / "trades_BBB.csv").write_text("\n".join(lines) + "\n")

    # Generic JSON quotes for BBB, timestamps in µs, numbers as JSON numbers.
    quotes = []
    t_us = T0_MS * 1000
    for i in range(10):
        t_us += 1_500_000
        quotes.append({"t": t_us, "b": 2500 + i * 0.5, "bq": 3, "a": 2501 + i * 0.5, "aq": 4})
    (OUT / "quotes_BBB.json").write_text(json.dumps(quotes, indent=1) + "\n")
    print(f"fixtures written to {OUT}")


if __name__ == "__main__":
    main()
