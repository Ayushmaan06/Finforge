"""Normalised Arrow schemas (PLAN Phase 3 task 1).

Units everywhere: timestamps are int64 ns since the Unix epoch, UTC (TIME-1); prices are int64
ticks and quantities int64 lots of the instrument (NUM-1, INS-1). Bars are labelled by open time
and emitted at `ts_close = ts_open + interval` (TIME-2).

`aggressor_side` is int8: 0 = buy, 1 = sell, -1 = unknown. A sentinel instead of a null keeps the
column non-nullable, so replay can hand it to C++ zero-copy.
"""

import pyarrow as pa

SCHEMA_VERSION = 1

KINDS = ("bars", "quotes", "trades")  # also the tie-break order for feeds of one instrument

_UNITS = "ts: int64 ns since epoch UTC; prices: int64 ticks; quantities: int64 lots"


def _schema(kind: str, fields: list[tuple[str, pa.DataType]]) -> pa.Schema:
    return pa.schema(
        [pa.field(n, t, nullable=False) for n, t in fields],
        metadata={
            "finforge.kind": kind,
            "finforge.schema_version": str(SCHEMA_VERSION),
            "finforge.units": _UNITS,
        },
    )


SCHEMAS: dict[str, pa.Schema] = {
    "bars": _schema(
        "bars",
        [
            ("ts_open", pa.int64()),
            ("ts_close", pa.int64()),
            ("instrument_id", pa.uint32()),
            ("open", pa.int64()),
            ("high", pa.int64()),
            ("low", pa.int64()),
            ("close", pa.int64()),
            ("volume", pa.int64()),
        ],
    ),
    "quotes": _schema(
        "quotes",
        [
            ("ts", pa.int64()),
            ("instrument_id", pa.uint32()),
            ("bid", pa.int64()),
            ("ask", pa.int64()),
            ("bid_qty", pa.int64()),
            ("ask_qty", pa.int64()),
        ],
    ),
    "trades": _schema(
        "trades",
        [
            ("ts", pa.int64()),
            ("instrument_id", pa.uint32()),
            ("price", pa.int64()),
            ("qty", pa.int64()),
            ("aggressor_side", pa.int8()),
        ],
    ),
}

# Column order the C++ feed expects (engine/include/finforge/replay.hpp).
REPLAY_COLUMNS: dict[str, tuple[str, ...]] = {
    "bars": ("ts_open", "ts_close", "open", "high", "low", "close", "volume"),
    "quotes": ("ts", "bid", "bid_qty", "ask", "ask_qty"),
    "trades": ("ts", "price", "qty", "aggressor_side"),
}

# The column a feed is ordered by in storage.
TIME_COLUMN = {"bars": "ts_open", "quotes": "ts", "trades": "ts"}
