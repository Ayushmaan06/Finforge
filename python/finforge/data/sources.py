"""Raw source readers (PLAN Phase 3 task 3).

Every reader streams a file as batches of *text* columns under canonical names, so the ingester can
convert values exactly (no float ever touches a price). Supported containers: CSV (plain or a
single-member .zip, as Binance publishes them), Parquet, and JSON (a list of objects; small files
only, it is read in one go).

Formats map source columns to canonical fields:

- ``binance_klines``: Binance spot/futures kline CSV (12 columns, header optional).
- ``binance_bookticker``: Binance USD-M futures bookTicker CSV (header optional).
- ``generic``: any container with a user-supplied ``columns`` mapping {canonical: source name}.
"""

import io
import json
import zipfile
from collections.abc import Iterator
from dataclasses import dataclass, field
from pathlib import Path

import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.csv as pacsv
import pyarrow.parquet as pq

BINANCE_KLINE_COLUMNS = [
    "open_time",
    "open",
    "high",
    "low",
    "close",
    "volume",
    "close_time",
    "quote_volume",
    "count",
    "taker_buy_volume",
    "taker_buy_quote_volume",
    "ignore",
]
BINANCE_BOOKTICKER_COLUMNS = [
    "update_id",
    "best_bid_price",
    "best_bid_qty",
    "best_ask_price",
    "best_ask_qty",
    "transaction_time",
    "event_time",
]

# canonical field -> source column, per fixed format
FORMATS: dict[str, dict] = {
    "binance_klines": {
        "kind": "bars",
        "names": BINANCE_KLINE_COLUMNS,
        "columns": {
            "ts_open": "open_time",
            "ts_close": "close_time",
            "open": "open",
            "high": "high",
            "low": "low",
            "close": "close",
            "volume": "volume",
        },
        "ts_units": ("ms", "us"),  # TIME-6: ms until 2024, µs for spot from 2025-01-01
        "close_time": "inclusive",  # Binance close_time = open_time + interval - 1 unit
    },
    "binance_bookticker": {
        "kind": "quotes",
        "names": BINANCE_BOOKTICKER_COLUMNS,
        "columns": {
            # The matching engine's time; event_time is when the message was sent.
            "ts": "transaction_time",
            "bid": "best_bid_price",
            "bid_qty": "best_bid_qty",
            "ask": "best_ask_price",
            "ask_qty": "best_ask_qty",
        },
        "ts_units": ("ms", "us"),
    },
}

CSV_BLOCK_BYTES = 4 << 20


@dataclass
class RawBatch:
    file: str  # display name (file name)
    first_row: int  # 1-based number of the first row among the file's well-formed data rows
    columns: dict[str, pa.Array]  # canonical name -> values (text, or integers)
    ts_unit_hint: str | None = None  # set when the container's type says the unit
    malformed_rows: list[int] = field(default_factory=list)  # physical line numbers

    @property
    def num_rows(self) -> int:
        return len(next(iter(self.columns.values()))) if self.columns else 0


def _open_text(path: Path) -> io.BufferedIOBase:
    if path.suffix == ".zip":
        zf = zipfile.ZipFile(path)
        members = [n for n in zf.namelist() if not n.endswith("/")]
        if len(members) != 1:
            raise ValueError(f"{path.name}: expected exactly one file in the zip, got {members}")
        return zf.open(members[0])
    return open(path, "rb")


def _first_line(path: Path) -> str:
    with _open_text(path) as f:
        return f.readline(1 << 16).decode("utf-8", "replace").strip()


def _looks_like_header(line: str) -> bool:
    first = line.split(",", 1)[0].strip().strip('"')
    return bool(first) and not first.lstrip("-").replace(".", "", 1).isdigit()


def _iter_csv(path: Path, names: list[str] | None, mapping: dict[str, str]) -> Iterator[RawBatch]:
    """`names`: fixed column names (header skipped if present); None = use the header."""
    header = _looks_like_header(_first_line(path))
    if names is None and not header:
        raise ValueError(f"{path.name}: generic CSV needs a header row")
    malformed: list[int] = []

    def on_invalid(row: pacsv.InvalidRow) -> str:
        malformed.append(row.number if row.number is not None and row.number >= 0 else -1)
        return "skip"

    read = pacsv.ReadOptions(
        use_threads=False,
        block_size=CSV_BLOCK_BYTES,
        column_names=names,
        skip_rows=1 if (names is not None and header) else 0,
    )
    source_cols = list(dict.fromkeys(mapping.values()))
    convert = pacsv.ConvertOptions(
        column_types={c: pa.string() for c in source_cols},
        include_columns=source_cols,
        strings_can_be_null=False,
    )
    parse = pacsv.ParseOptions(invalid_row_handler=on_invalid)
    row = 1
    with _open_text(path) as f:
        try:
            reader = pacsv.open_csv(
                f, read_options=read, parse_options=parse, convert_options=convert
            )
        except pa.ArrowInvalid as e:
            raise ValueError(f"{path.name}: {e}") from e
        for batch in reader:
            cols = {canon: batch.column(src) for canon, src in mapping.items()}
            yield RawBatch(path.name, row, cols, malformed_rows=malformed[:])
            malformed.clear()
            row += batch.num_rows
    if malformed:  # malformed rows after the last batch
        yield RawBatch(
            path.name, row, {c: pa.array([], pa.string()) for c in mapping}, None, malformed
        )


def _iter_parquet(path: Path, mapping: dict[str, str], batch_rows: int) -> Iterator[RawBatch]:
    pf = pq.ParquetFile(path)
    source_cols = list(dict.fromkeys(mapping.values()))
    missing = [c for c in source_cols if c not in pf.schema_arrow.names]
    if missing:
        raise ValueError(f"{path.name}: missing columns {missing}")
    row = 1
    for batch in pf.iter_batches(batch_size=batch_rows, columns=source_cols, use_threads=False):
        cols: dict[str, pa.Array] = {}
        hint = None
        for canon, src in mapping.items():
            arr = batch.column(src)
            if pa.types.is_timestamp(arr.type):
                hint = {"s": "s", "ms": "ms", "us": "us", "ns": "ns"}[arr.type.unit]
                arr = pc.cast(arr, pa.int64())
            cols[canon] = arr
        yield RawBatch(path.name, row, cols, ts_unit_hint=hint)
        row += batch.num_rows


def _iter_json(path: Path, mapping: dict[str, str]) -> Iterator[RawBatch]:
    # parse_float/parse_int keep the exact source text of every number.
    with _open_text(path) as f:
        rows = json.load(f, parse_float=str, parse_int=str)
    if not isinstance(rows, list):
        raise ValueError(f"{path.name}: JSON source must be a list of objects")

    def text(v: object) -> str | None:
        if v is None:
            return None
        if isinstance(v, bool):
            return "true" if v else "false"
        return str(v)

    cols = {
        canon: pa.array([text(r.get(src)) for r in rows], pa.string())
        for canon, src in mapping.items()
    }
    yield RawBatch(path.name, 1, cols)


def iter_source(
    path: Path, fmt: str, mapping: dict[str, str] | None, batch_rows: int
) -> Iterator[RawBatch]:
    """Stream one source file as canonical text columns."""
    if fmt in FORMATS:
        spec = FORMATS[fmt]
        return _iter_csv(path, spec["names"], spec["columns"])
    if fmt != "generic":
        raise ValueError(f"unknown format {fmt!r}")
    if not mapping:
        raise ValueError("generic sources need a 'columns' mapping")
    name = path.name.lower()
    if name.endswith((".csv", ".csv.zip", ".zip")):
        return _iter_csv(path, None, mapping)
    if name.endswith(".parquet"):
        return _iter_parquet(path, mapping, batch_rows)
    if name.endswith(".json"):
        return _iter_json(path, mapping)
    raise ValueError(f"{path.name}: unsupported container (csv, zip, parquet, json)")
