# Historical data pipeline

How raw market data becomes a validated, versioned dataset, and how a dataset is replayed through
the engine. This is the reference for PLAN.md Phase 3. Rules cited as TIME-n, INS-n, EVT-n come
from [contracts.md](contracts.md).

```text
raw files ──► ingest (Python, pyarrow) ──────────────────────► dataset directory
 CSV / zip      read as text → exact fixed point → checks        manifest.json
 Parquet        → policy (reject / drop / flag) → row groups     validation.json
 JSON                                                            <kind>/<symbol>.parquet

dataset ──► replay: iter_batches (one chunk per feed) ──zero-copy──► C++ Replay (k-way merge)
                                                                       └─► Simulator, one event at a time
```

## 1. Normalised schemas

Timestamps are int64 nanoseconds since the Unix epoch, UTC (TIME-1). Prices are int64 **ticks** and
quantities int64 **lots** of the instrument (NUM-1, INS-1). All columns are non-nullable.

| Kind | Columns | Emitted at |
|---|---|---|
| `bars` | `ts_open, ts_close, instrument_id, open, high, low, close, volume` | `ts_close = ts_open + interval` (TIME-2) |
| `quotes` | `ts, instrument_id, bid, ask, bid_qty, ask_qty` | `ts` (TIME-4) |
| `trades` | `ts, instrument_id, price, qty, aggressor_side` | `ts` (TIME-4) |

`instrument_id` is uint32 and `aggressor_side` is int8 (0 buy, 1 sell, −1 unknown). The other
columns are int64. The sentinel −1 keeps the column non-nullable, so it can be handed to C++ without
a copy. Schema metadata carries `finforge.kind` and `finforge.schema_version` (currently 1).
Definitions: `python/finforge/data/schema.py`.

## 2. Dataset spec and registry

A dataset is described by a JSON spec. Paths are relative to the spec's directory, and glob
patterns are expanded in sorted order. The specs for the PLAN D5 data are in `datasets/specs/`.

```json
{
  "name": "binance-spot-1m-2023-2024",
  "notes": ["shown by `info` and kept in the manifest"],
  "instruments": [
    {"symbol": "BTCUSDT", "asset_class": "spot", "quote_currency": "USDT",
     "tick_size": "0.01", "lot_size": "0.000001", "min_qty": 1, "contract_multiplier": 1}
  ],
  "policies": {"bars": {"BAD_CLOSE_TIME": "flag"}},
  "chunk_rows": 65536,
  "sources": [
    {"format": "binance_klines", "symbol": "BTCUSDT", "interval": "1m",
     "paths": ["../raw/binance/spot/klines/BTCUSDT/1m/BTCUSDT-1m-202[34]-*.zip"]}
  ]
}
```

- **Registry (INS-1, INS-2).** Instrument ids are dense from 0 in list order. `tick_size` and
  `lot_size` must be decimal strings; floats are refused. Each instrument passes the engine's own
  NUM-2 check (`finforge._engine.money_per_tick_lot`), so Python and C++ cannot disagree. The registry
  can also live in a separate file (`"registry": "registry.json"`). It is copied into the manifest.
- **One source per (kind, symbol).** Months or days of one stream go in that source's `paths`, and
  are read in order with continuity checks across file boundaries.
- `chunk_rows` sets the Parquet row-group size and the default replay chunk.

## 3. Source formats

Every value is read as **text** and converted exactly. A price is never parsed as a float (§4).

| `format` | Kind | Layout | Timestamp |
|---|---|---|---|
| `binance_klines` | bars | 12 columns; header optional (spot files have none, futures files do) | `open_time`; `close_time` must equal `open_time + interval − 1 unit` |
| `binance_bookticker` | quotes | USD-M futures bookTicker; header optional | `transaction_time` (matching-engine time; `event_time` is ignored) |
| `generic` | any | CSV with a header, a single-file `.zip`, Parquet, or JSON (a list of objects; small files only) | mapped column |

`generic` sources need a `columns` mapping `{canonical: source name}`:

- bars: `ts_open, open, high, low, close, volume`; optional `ts_close`, with
  `close_time` `"exclusive"` (default, `= open + interval`) or `"inclusive"`.
- quotes: `ts, bid, bid_qty, ask, ask_qty`.
- trades: `ts, price, qty`; optional `aggressor_side`. With `"aggressor": "side"` (the default) it
  takes `buy`/`b`/`sell`/`s`, case-insensitive. With `"is_buyer_maker"` it takes `true`/`false`. An
  empty value means unknown.

How each container is turned into text:

- **Parquet:** numbers are cast to text first. Doubles use their shortest round-trip form, so `0.1`
  becomes exactly 10 ticks of 0.01, never 9. Arrow `timestamp` columns take their unit from the type.
- **JSON:** numbers keep their exact source text (`parse_float=str`).

**Timestamp units (TIME-6).** The plausible ranges for 2000–2100 in seconds, milliseconds,
microseconds and nanoseconds do not overlap, so a value's magnitude identifies its unit. The first
usable value fixes the unit for the whole file. Any value outside that unit's range is reported as
`TS_UNIT_AMBIGUOUS`, and the dataset is rejected. The two Binance formats allow ms and µs only;
Binance spot files switched to µs on 2025-01-01. Generic sources take `"ts_unit": "auto"` (the
default) or one fixed unit.

## 4. Exact conversion

`finforge.data.fixedpoint` uses only vectorised pyarrow string kernels:

1. Trim the value and match `-?digits(.digits)?`. Anything else is `BAD_NUMBER`; empty is `MISSING_VALUE`.
2. Strip trailing zeros from the fraction. More significant digits than the tick/lot scale means the
   value is off the grid (`OFF_TICK` / `OFF_LOT`).
3. Right-pad the fraction to the scale, join it to the integer digits, and check the result against
   `9223372036854775807`. Values that do not fit are `BAD_NUMBER`; values are never wrapped.
4. Cast to int64, then divide by the grid step in scale units (for example, tick `0.5` is 5 at scale
   1). A non-zero remainder is off the grid.

## 5. Validation

Validation never stops at the first problem. Every problem is counted per source, with up to five
sample rows (`file`, 1-based data row; malformed CSV rows use the physical line number). Each code
has an **action**, set per dataset kind in the spec's `policies`:

- `reject`: the ingest fails and nothing is written.
- `drop`: the rows are removed and counted.
- `flag`: the rows are kept and counted.

| Code | Meaning | Allowed | Default (bars / quotes / trades) |
|---|---|---|---|
| `TS_UNIT_AMBIGUOUS` | timestamps not all in one allowed unit (TIME-6) | reject | reject |
| `MALFORMED_ROW` | wrong number of CSV columns | reject, drop | reject |
| `MISSING_VALUE` | a required field is empty | reject, drop | reject |
| `BAD_NUMBER` | not a plain decimal, or does not fit int64 | reject, drop | reject |
| `OFF_TICK` / `OFF_LOT` | not a whole number of ticks / lots | reject, drop | reject |
| `NON_POSITIVE_PRICE` | price ≤ 0 | any | reject |
| `NEGATIVE_QTY` | quantity or volume < 0 | any | reject |
| `OHLC_INCONSISTENT` | not `low ≤ open, close ≤ high` | any | reject |
| `CROSSED_QUOTE` | bid > ask (locked quotes are fine) | any | reject / **drop** / – |
| `BAD_CLOSE_TIME` | source close time ≠ open + interval | any | reject |
| `BAD_VALUE` | unrecognised aggressor (kept as unknown when flagged) | any | reject |
| `NON_MONOTONIC_TS` | earlier than an earlier row | reject, drop | reject |
| `DUPLICATE` | bars: same open time; quotes/trades: identical row | any | reject / **flag** / **flag** |
| `MISALIGNED_BAR` | open time not a multiple of the interval | any | reject |
| `GAP` | bars: more than one interval since the previous bar (reports `missing_intervals`); quotes/trades: longer than `max_gap`, if set | reject, flag | **flag** |

The checks run in this order:

1. Row-level checks.
2. Rows with no usable value are removed.
3. Sequence checks (`NON_MONOTONIC_TS`, then `DUPLICATE`), using state carried across batches and
   files.

`GAP` is measured over every row with a usable timestamp, before value-level drops, so it describes
the source's coverage. A row out of order is not also reported as a gap.

The report status is `clean` (no findings), `flagged` (findings, none rejecting) or `rejected`.

### Committed fixtures

`datasets/fixtures/` holds synthetic files in the Binance layouts (regenerate them with
`python scripts/make_fixtures.py`). Each malformed fixture's expected findings are pinned in
`python/tests/test_data_ingest.py`.

| Fixture | Findings |
|---|---|
| `klines_AAA_ms.csv`, `klines_BBB_ms.csv` (header), `klines_AAA_us.csv` (2025, µs), `bookticker_AAA.csv`, `quotes_BBB.json` | none |
| `klines_ambiguous.csv` | `TS_UNIT_AMBIGUOUS` × 30 (30 ms rows, then 30 µs rows) |
| `klines_ohlc.csv` | `OHLC_INCONSISTENT` × 2 |
| `klines_sequence.csv` | `DUPLICATE` × 1, `NON_MONOTONIC_TS` × 1 |
| `klines_gap.csv` | `GAP` × 1 (3 missing intervals) |
| `klines_grid.csv` | `OFF_TICK` × 1, `OFF_LOT` × 1 |
| `klines_values.csv` | one each of `MALFORMED_ROW`, `MISSING_VALUE`, `BAD_NUMBER`, `NEGATIVE_QTY`, `NON_POSITIVE_PRICE`, `BAD_CLOSE_TIME` |
| `bookticker_dirty.csv` | `CROSSED_QUOTE` × 2, `DUPLICATE` × 1, `NON_MONOTONIC_TS` × 1 |
| `trades_BBB.csv` (generic) | `BAD_VALUE` × 1 |

## 6. Dataset layout, manifest and id

```text
<dataset>/manifest.json           format, schema_version, id, name, instruments, files,
                                  time_range_ns, chunk_rows, sources, validation summary, notes
<dataset>/validation.json         the full report, with sample rows
<dataset>/<kind>/<symbol>.parquet row groups of chunk_rows rows
```

- Files are listed in (instrument id, bars < quotes < trades) order. That order is also the replay
  tie-break.
- `sources` records each input file's SHA-256, size, row count and detected timestamp unit.
- The manifest holds no wall-clock time, so ingesting the same inputs twice gives a byte-identical
  manifest. This is tested, and was checked on the real D5 data (§8).
- The output is written to a temporary directory and renamed only on success. A rejected ingest
  leaves nothing behind.

**Dataset id (VER-1, deviation from PLAN D6).** The id is a SHA-256 of the *logical* content, not of
the Parquet bytes, because those depend on the pyarrow version and encoder settings. It is built in
three steps:

1. For each file, take the SHA-256 of each column's little-endian values.
2. Combine those with the kind, schema version and row count into the file's `content_sha256`.
3. Hash the schema version, the registry, and each file's (kind, instrument, rows,
   `content_sha256`, interval) into the dataset `id`.

The id therefore does not depend on `chunk_rows` or row-group layout. `Dataset.verify()`
(`python -m finforge.data verify`) recomputes everything and reports tampering.

## 7. Replay

`finforge.data.replay(dataset, chunk_rows=…)` builds an engine from the manifest's registry and
attaches one feed per file. Each feed is an iterator over `ParquetFile.iter_batches(batch_size=
chunk_rows)`, and each batch's columns go to C++ as numpy views of the Arrow buffers.

- **Zero-copy boundary.** The C++ side (`python/bindings/module.cpp`) accepts only 1-D,
  C-contiguous int64 arrays (int8 for aggressors). Anything else raises `TypeError` rather than
  being copied silently. The feed keeps a reference to the batch until it moves to the next one.
- **Merge (EVT-2).** `finforge::Replay` (`engine/include/finforge/replay.hpp`) is a heap over the
  feeds, keyed by (emit time, instrument id, attach order). Rows within a feed keep their order. A
  feed whose emit times go backwards raises `ValueError`.
- **Streaming into the simulator.** `Simulator::attach_source` keeps at most one source event in
  the queue and pulls the next one when it is processed. Memory is bounded by one chunk per feed,
  whatever the dataset size. Pre-scheduled market data (`schedule_market_data`) and the Phase 2
  fingerprint are unchanged.
- Phase 3 replays with a no-op strategy. Strategies arrive in Phase 5. The Python GIL is held for
  the whole run, because the chunk iterators are Python code.

Measured in the tests:

- **Merge order:** every row of a 2-instrument fixture dataset (950 events, including
  same-timestamp ties across instruments and kinds) comes out in exactly the documented order.
- **Chunk size:** the journal hash is identical for chunk sizes 1, 7, 64 and 65 536.
- **Memory:** replaying 3 M quotes (137 MiB of column data) raises peak RSS by about 34 MiB with
  64k-row chunks, against about 152 MiB when each file is read as one chunk.

## 8. The D5 datasets

```bash
python scripts/fetch_binance.py            # all of D5; checksum-verified, re-runnable
python -m finforge.data ingest datasets/specs/binance-spot-1m.json --out datasets/normalized/binance-spot-1m
python -m finforge.data info   datasets/normalized/binance-spot-1m
python -m finforge.data verify datasets/normalized/binance-spot-1m
python -m finforge.data replay datasets/normalized/binance-spot-1m
```

**Spot 1-minute klines, BTCUSDT + ETHUSDT, 2023–2024**

- 2 105 120 bars, which is 2 × (731 days × 1 440 − 80) minutes.
- Ingest takes about 16 s; replay takes about 2 s (about 1 M events/s).
- Dataset id `256e5d6e15778ad98ed39bbe2f2dead5aa201fff95d2bf90b794a553d490096e`, stable across runs.
- The report is **flagged, as expected**. Both symbols have an 80-minute `GAP` and one
  `BAD_CLOSE_TIME`, both from the Binance trading halt on 2023-03-24: the last bar before the halt
  closes early in the source.

Two facts about the source data shaped the spec:

- BTCUSDT volumes in 2024 sometimes carry six decimals. The registry therefore uses lot 0.000001,
  not 0.00001. With the coarser lot, 121 rows were `OFF_LOT`.
- The halt's short bar is a real partial bar, so `BAD_CLOSE_TIME` is flagged for this dataset. The
  bar is kept and emitted at its regular close time.

**USD-M futures BTCUSDT bookTicker + 1-minute klines, 2024-03-01 and 2024-03-02**
(`datasets/specs/binance-futures-btc-quotes.json`, INS-4: perpetual treated as spot-like)

- 67 109 592 quotes and 2 880 bars, from 67 112 748 source rows.
- Ingest takes about 3 min with a peak RSS of about 390 MiB, streaming 680 MB of zips.
- Replay of 67.1 M events takes 44 s (about 1.5 M events/s) with a peak RSS of about 140 MiB.
- Dataset id `1f6f2ce84408ef37c375a8595bb8b217c341a9de0b8f5414d91eab0b34f67753`. The output is
  445 MB of Parquet.
- The report is **flagged**: 276 quotes have asks off the 0.10 tick in the source (for example
  `61431.87`). They are dropped under this dataset's policy, because an off-grid price has no exact
  representation. There are no crossed quotes and no out-of-order timestamps.
