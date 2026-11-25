"""Ingestion: raw sources → validated, normalised, versioned dataset (PLAN Phase 3 tasks 3-5).

Everything streams: a source is read batch by batch, each batch is converted exactly, checked, and
appended to the output in row groups of ``chunk_rows``. State that spans batches and files (last
timestamp, last row, timestamp unit of the current file) is carried, so results do not depend on
batch boundaries.

Output layout::

    <out>/manifest.json          id, registry, files, time range, validation summary (no wall clock)
    <out>/validation.json        the full report, with sample rows
    <out>/<kind>/<symbol>.parquet

The dataset id is a SHA-256 over the *logical* content (see ``dataset_id``), not over Parquet bytes,
which depend on the pyarrow version (deviation from PLAN D6, documented in docs/data.md).
"""

import glob
import hashlib
import json
import os
import re
import shutil
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

from finforge.data.fixedpoint import Parsed, parse_int, parse_on_grid
from finforge.data.registry import Instrument, Registry
from finforge.data.schema import KINDS, SCHEMA_VERSION, SCHEMAS, TIME_COLUMN
from finforge.data.sources import FORMATS, RawBatch, iter_source
from finforge.data.validate import DROP, DatasetRejected, Report, SourceStats, resolve_policy

DEFAULT_CHUNK_ROWS = 65_536

# TIME-6: plausible range 2000-01-01 .. 2100-01-01 in each unit. The ranges do not overlap, so a
# value's magnitude identifies its unit.
_LO_S, _HI_S = 946_684_800, 4_102_444_800
_PER_SECOND = {"s": 1, "ms": 1_000, "us": 1_000_000, "ns": 1_000_000_000}
_NS_PER_UNIT = {u: 1_000_000_000 // p for u, p in _PER_SECOND.items()}
ALL_UNITS = ("s", "ms", "us", "ns")

_INTERVAL = re.compile(r"^([1-9][0-9]*)(s|m|h|d)$")
_INTERVAL_NS = {"s": 10**9, "m": 60 * 10**9, "h": 3600 * 10**9, "d": 86400 * 10**9}
_SAFE_SYMBOL = re.compile(r"^[A-Za-z0-9._-]+$")


def parse_interval(text: str) -> int:
    m = _INTERVAL.match(text)
    if not m:
        raise ValueError(f"interval must look like 1m, 5s, 1h, 1d; got {text!r}")
    return int(m.group(1)) * _INTERVAL_NS[m.group(2)]


def unit_of(values: np.ndarray) -> np.ndarray:
    """Index into ALL_UNITS of each value's unit, or -1 when no plausible unit fits."""
    out = np.full(values.shape, -1, dtype=np.int8)
    for i, u in enumerate(ALL_UNITS):
        lo, hi = _LO_S * _PER_SECOND[u], _HI_S * _PER_SECOND[u]
        out[(values >= lo) & (values < hi)] = i
    return out


# --- Spec ---------------------------------------------------------------------------------------


@dataclass
class SourceSpec:
    kind: str
    symbol: str
    format: str
    paths: list[Path]
    columns: dict[str, str] = field(default_factory=dict)
    interval_ns: int | None = None  # bars
    ts_units: tuple[str, ...] = ALL_UNITS
    close_time: str | None = None  # bars with a source close column: "inclusive" | "exclusive"
    aggressor: str = "side"  # trades: "side" (buy/sell) | "is_buyer_maker" (true/false)
    max_gap_ns: int | None = None  # quotes/trades: report gaps longer than this


@dataclass
class DatasetSpec:
    name: str
    registry: Registry
    sources: list[SourceSpec]
    chunk_rows: int = DEFAULT_CHUNK_ROWS
    policies: dict[str, dict[str, str]] = field(default_factory=dict)
    notes: list[str] = field(default_factory=list)

    @classmethod
    def from_dict(cls, d: dict[str, Any], root: Path) -> "DatasetSpec":
        if "registry" in d:
            registry = Registry.load(root / d["registry"])
        else:
            registry = Registry.from_dicts(d["instruments"])
        sources = [_source_from_dict(s, root, registry) for s in d["sources"]]
        keys = [(s.kind, s.symbol) for s in sources]
        if len(set(keys)) != len(keys):
            raise ValueError("one source per (kind, symbol); list several paths in one source")
        policies = d.get("policies", {})
        for kind in policies:
            if kind not in KINDS:
                raise ValueError(f"policies: unknown kind {kind!r}")
            resolve_policy(kind, policies[kind])  # validates
        chunk_rows = int(d.get("chunk_rows", DEFAULT_CHUNK_ROWS))
        if chunk_rows < 1:
            raise ValueError("chunk_rows must be >= 1")
        return cls(
            name=str(d["name"]),
            registry=registry,
            sources=sources,
            chunk_rows=chunk_rows,
            policies=policies,
            notes=list(d.get("notes", [])),
        )

    @classmethod
    def load(cls, path: str | Path, root: str | Path | None = None) -> "DatasetSpec":
        path = Path(path)
        return cls.from_dict(json.loads(path.read_text()), Path(root) if root else path.parent)


def _source_from_dict(s: dict[str, Any], root: Path, registry: Registry) -> SourceSpec:
    fmt = s.get("format", "generic")
    kind = FORMATS[fmt]["kind"] if fmt in FORMATS else s["kind"]
    if s.get("kind", kind) != kind:
        raise ValueError(f"format {fmt} produces {kind}, not {s['kind']}")
    if kind not in KINDS:
        raise ValueError(f"unknown kind {kind!r}")
    symbol = s["symbol"]
    registry.by_symbol(symbol)
    if not _SAFE_SYMBOL.match(symbol):
        raise ValueError(f"symbol {symbol!r} is not usable as a file name")

    paths: list[Path] = []
    for pattern in s["paths"]:
        full = str(root / pattern)
        matches = sorted(glob.glob(full)) if glob.has_magic(full) else [full]
        if not matches:
            raise FileNotFoundError(f"no files match {full}")
        paths.extend(Path(m) for m in matches)
    for p in paths:
        if not p.is_file():
            raise FileNotFoundError(p)

    if fmt in FORMATS:
        columns = dict(FORMATS[fmt]["columns"])
        units = tuple(FORMATS[fmt]["ts_units"])
        close = FORMATS[fmt].get("close_time")
    else:
        columns = dict(s["columns"])
        unit = s.get("ts_unit", "auto")
        units = ALL_UNITS if unit == "auto" else (unit,)
        if any(u not in ALL_UNITS for u in units):
            raise ValueError(f"ts_unit must be auto or one of {ALL_UNITS}")
        close = s.get("close_time", "exclusive") if "ts_close" in columns else None
    required = {
        "bars": {"ts_open", "open", "high", "low", "close", "volume"},
        "quotes": {"ts", "bid", "bid_qty", "ask", "ask_qty"},
        "trades": {"ts", "price", "qty"},
    }[kind]
    allowed = required | {"bars": {"ts_close"}, "quotes": set(), "trades": {"aggressor_side"}}[kind]
    if not required <= set(columns) or not set(columns) <= allowed:
        raise ValueError(
            f"{kind} columns must cover {sorted(required)} (optional: {sorted(allowed - required)})"
        )

    interval_ns = parse_interval(s["interval"]) if kind == "bars" else None
    if kind == "bars" and "interval" not in s:
        raise ValueError("bars sources need an 'interval'")
    max_gap = s.get("max_gap")
    return SourceSpec(
        kind=kind,
        symbol=symbol,
        format=fmt,
        paths=paths,
        columns=columns,
        interval_ns=interval_ns,
        ts_units=units,
        close_time=close,
        aggressor=s.get("aggressor", "side"),
        max_gap_ns=parse_interval(max_gap) if max_gap else None,
    )


# --- Output -------------------------------------------------------------------------------------


class _Writer:
    """Buffers normalised rows and writes row groups of exactly `chunk_rows` (except the last).
    Also hashes every column's little-endian bytes, independent of batching."""

    def __init__(self, path: Path | None, kind: str, chunk_rows: int):
        self.kind = kind
        self.schema = SCHEMAS[kind]
        self.chunk_rows = chunk_rows
        self.path = path
        self._writer = pq.ParquetWriter(path, self.schema) if path else None
        self._buf: list[dict[str, np.ndarray]] = []
        self._buffered = 0
        self._hash = {f.name: hashlib.sha256() for f in self.schema}
        self.rows = 0
        self.ts_min: int | None = None
        self.ts_max: int | None = None  # of emit times
        self.emit_max: int | None = None

    def append(self, cols: dict[str, np.ndarray]) -> None:
        n = len(cols[TIME_COLUMN[self.kind]])
        if n == 0:
            return
        for f in self.schema:
            le = np.dtype(f.type.to_pandas_dtype()).newbyteorder("<")
            self._hash[f.name].update(np.ascontiguousarray(cols[f.name], dtype=le).tobytes())
        t = cols[TIME_COLUMN[self.kind]]
        emit = cols["ts_close"] if self.kind == "bars" else t
        self.ts_min = int(t.min()) if self.ts_min is None else min(self.ts_min, int(t.min()))
        self.ts_max = int(t.max()) if self.ts_max is None else max(self.ts_max, int(t.max()))
        e = int(emit.max())
        self.emit_max = e if self.emit_max is None else max(self.emit_max, e)
        self.rows += n
        self._buf.append(cols)
        self._buffered += n
        while self._buffered >= self.chunk_rows:
            self._flush(self.chunk_rows)

    def _flush(self, n: int) -> None:
        merged = {k: np.concatenate([b[k] for b in self._buf]) for k in self._buf[0]}
        if self._writer is not None:
            table = pa.Table.from_arrays(
                [pa.array(merged[f.name][:n], f.type) for f in self.schema], schema=self.schema
            )
            self._writer.write_table(table, row_group_size=n)
        rest = {k: v[n:] for k, v in merged.items()}
        self._buffered -= n
        self._buf = [rest] if self._buffered else []

    def close(self) -> None:
        if self._buffered:
            self._flush(self._buffered)
        if self._writer is not None:
            self._writer.close()

    def content_sha256(self) -> str:
        h = hashlib.sha256()
        meta = {
            "kind": self.kind,
            "schema_version": SCHEMA_VERSION,
            "rows": self.rows,
            "columns": [[f.name, str(f.type), self._hash[f.name].hexdigest()] for f in self.schema],
        }
        h.update(_canonical(meta))
        return h.hexdigest()


def _canonical(obj: Any) -> bytes:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()


def _file_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


# --- Per-source conversion and checks -----------------------------------------------------------


class _SourceIngest:
    def __init__(self, src: SourceSpec, inst: Instrument, policy: dict[str, str], report: Report):
        self.src = src
        self.inst = inst
        self.tick = inst.tick
        self.lot = inst.lot
        self.policy = policy
        self.report = report
        self.stats = SourceStats(kind=src.kind, symbol=src.symbol)
        report.sources.append(self.stats)
        self.prev_t: int | None = None  # latest time column value of the kept rows
        self.gap_prev_t: int | None = None  # latest usable source time (gap detection)
        self.prev_row: tuple | None = None  # last kept row (duplicate detection)
        self.file_unit: str | None = None
        self.unit_decided = False  # the first usable value of the file decides, once

    def start_file(self) -> None:
        self.file_unit = None  # TIME-6: detected per file
        self.unit_decided = False

    def _flag(self, code: str, mask: np.ndarray, rows: np.ndarray, file: str, **extra: int) -> None:
        if mask.any():
            self.report.add(
                code, self.src.kind, self.src.symbol, self.policy[code], rows[mask], file, **extra
            )

    def _price(self, col: pa.Array) -> Parsed:
        return parse_on_grid(col, self.tick)

    def _qty(self, col: pa.Array) -> Parsed:
        return parse_on_grid(col, self.lot)

    def process(self, raw: RawBatch) -> dict[str, np.ndarray] | None:
        src, kind = self.src, self.src.kind
        file = raw.file
        if raw.malformed_rows:
            self.report.add(
                "MALFORMED_ROW",
                kind,
                src.symbol,
                self.policy["MALFORMED_ROW"],
                raw.malformed_rows,
                file,
            )
        n = raw.num_rows
        if n == 0:
            return None
        self.stats.rows_read += n
        rows = raw.first_row + np.arange(n, dtype=np.int64)
        c = raw.columns

        # Timestamps (TIME-1, TIME-6).
        tcol = "ts_open" if kind == "bars" else "ts"
        ts = parse_int(c[tcol])
        ts_parts = [ts]
        src_close = parse_int(c["ts_close"]) if (kind == "bars" and "ts_close" in c) else None
        if src_close is not None:
            ts_parts.append(src_close)
        missing = np.zeros(n, dtype=bool)
        bad = np.zeros(n, dtype=bool)
        for p in ts_parts:
            missing |= p.missing
            bad |= p.bad

        allowed = (raw.ts_unit_hint,) if raw.ts_unit_hint else src.ts_units
        unit_idx = unit_of(ts.values)
        usable_ts = ~(ts.missing | ts.bad)
        ambiguous = np.zeros(n, dtype=bool)
        if usable_ts.any():
            if not self.unit_decided:
                self.unit_decided = True
                first = int(unit_idx[np.argmax(usable_ts)])
                if first >= 0 and ALL_UNITS[first] in allowed:
                    self.file_unit = ALL_UNITS[first]
            want = ALL_UNITS.index(self.file_unit) if self.file_unit else -2
            ambiguous = usable_ts & (unit_idx != want)
            if src_close is not None:
                ambiguous |= ~(src_close.missing | src_close.bad) & (
                    unit_of(src_close.values) != want
                )
        self._flag("TS_UNIT_AMBIGUOUS", ambiguous, rows, file)
        factor = _NS_PER_UNIT[self.file_unit] if self.file_unit else 1
        t_ns = ts.values * factor

        # Values on the tick / lot grid (NUM-1, INS-1).
        out: dict[str, np.ndarray] = {}
        prices: list[Parsed] = []
        qtys: list[Parsed] = []
        if kind == "bars":
            for name in ("open", "high", "low", "close"):
                p = self._price(c[name])
                prices.append(p)
                out[name] = p.values
            v = self._qty(c["volume"])
            qtys.append(v)
            out["volume"] = v.values
        elif kind == "quotes":
            for name in ("bid", "ask"):
                p = self._price(c[name])
                prices.append(p)
                out[name] = p.values
            for name in ("bid_qty", "ask_qty"):
                q = self._qty(c[name])
                qtys.append(q)
                out[name] = q.values
        else:
            p = self._price(c["price"])
            prices.append(p)
            out["price"] = p.values
            q = self._qty(c["qty"])
            qtys.append(q)
            out["qty"] = q.values
        for p in prices + qtys:
            missing |= p.missing
            bad |= p.bad
        off_tick = np.zeros(n, dtype=bool)
        for p in prices:
            off_tick |= p.off_grid
        off_lot = np.zeros(n, dtype=bool)
        for q in qtys:
            off_lot |= q.off_grid
        bad &= ~missing
        off_tick &= ~(missing | bad)
        off_lot &= ~(missing | bad)
        self._flag("MISSING_VALUE", missing, rows, file)
        self._flag("BAD_NUMBER", bad, rows, file)
        self._flag("OFF_TICK", off_tick, rows, file)
        self._flag("OFF_LOT", off_lot, rows, file)
        unusable = missing | bad | off_tick | off_lot | ambiguous
        ok = ~unusable

        drop = unusable.copy()

        def check(code: str, mask: np.ndarray, **extra: int) -> None:
            mask = mask & ok
            self._flag(code, mask, rows, file, **extra)
            if self.policy[code] == DROP:
                drop[:] |= mask

        # Value checks.
        if kind == "bars":
            o, h, lo, cl = out["open"], out["high"], out["low"], out["close"]
            check("NON_POSITIVE_PRICE", (o <= 0) | (h <= 0) | (lo <= 0) | (cl <= 0))
            check("NEGATIVE_QTY", out["volume"] < 0)
            check(
                "OHLC_INCONSISTENT", (lo > np.minimum(o, cl)) | (h < np.maximum(o, cl)) | (lo > h)
            )
            interval = src.interval_ns
            out["ts_open"] = t_ns
            out["ts_close"] = t_ns + interval
            if src_close is not None and self.file_unit:
                unit_ns = _NS_PER_UNIT[self.file_unit]
                if interval % unit_ns != 0:
                    raise ValueError(f"interval is not a whole number of {self.file_unit}")
                expected = (
                    ts.values + interval // unit_ns - (1 if src.close_time == "inclusive" else 0)
                )
                check("BAD_CLOSE_TIME", src_close.values != expected)
            check("MISALIGNED_BAR", t_ns % interval != 0)
        elif kind == "quotes":
            check("NON_POSITIVE_PRICE", (out["bid"] <= 0) | (out["ask"] <= 0))
            check("NEGATIVE_QTY", (out["bid_qty"] < 0) | (out["ask_qty"] < 0))
            check("CROSSED_QUOTE", out["bid"] > out["ask"])
            out["ts"] = t_ns
        else:
            check("NON_POSITIVE_PRICE", out["price"] <= 0)
            check("NEGATIVE_QTY", out["qty"] < 0)
            out["ts"] = t_ns
            out["aggressor_side"] = self._aggressor(
                c.get("aggressor_side"), n, ok, rows, file, drop
            )

        ts_ok = ~(ts.missing | ts.bad | ambiguous)
        self._gaps(out[TIME_COLUMN[kind]][ts_ok], rows[ts_ok], file)

        keep = ~drop
        rows = rows[keep]
        out = {k: v[keep] for k, v in out.items()}
        out = self._sequence(out, rows, file)
        m = len(out[TIME_COLUMN[kind]])
        out["instrument_id"] = np.full(m, self.inst.instrument_id, dtype=np.uint32)
        self.stats.rows_written += m
        return out

    def _aggressor(self, col, n, ok, rows, file, drop) -> np.ndarray:
        side = np.full(n, -1, dtype=np.int8)
        if col is None:
            return side
        s = pc.utf8_lower(pc.utf8_trim_whitespace(pc.cast(col, pa.string()))).fill_null("")
        vals = np.asarray(s.to_numpy(zero_copy_only=False), dtype=object)
        if self.src.aggressor == "is_buyer_maker":
            buy, sell = {"false", "0"}, {"true", "1"}  # buyer is maker => seller aggressed
        else:
            buy, sell = {"buy", "b"}, {"sell", "s"}
        is_buy = np.isin(vals, list(buy))
        is_sell = np.isin(vals, list(sell))
        unknown = vals == ""
        bad = ~(is_buy | is_sell | unknown) & ok
        side[is_buy] = 0
        side[is_sell] = 1
        self._flag("BAD_VALUE", bad, rows, file)
        if self.policy["BAD_VALUE"] == DROP:
            drop |= bad
        return side

    def _gaps(self, t: np.ndarray, rows: np.ndarray, file: str) -> None:
        """GAP (flag or reject only): time since the latest earlier row, over every row with a
        usable timestamp, so it describes the source's coverage. Rows dropped for value problems
        do not create gaps, and a row out of order (NON_MONOTONIC_TS) does not look like one."""
        if len(t) == 0:
            return
        start = t[0] if self.gap_prev_t is None else self.gap_prev_t
        prev_t = np.maximum.accumulate(np.concatenate([[start], t]))[:-1]
        diff = t - prev_t
        src = self.src
        if src.kind == "bars":
            gap = diff > src.interval_ns
            missing = int(np.sum(diff[gap] // src.interval_ns - 1))
            self._flag("GAP", gap, rows, file, missing_intervals=missing)
        elif src.max_gap_ns is not None:
            self._flag("GAP", diff > src.max_gap_ns, rows, file)
        self.gap_prev_t = int(max(start, t.max()))

    def _sequence(
        self, out: dict[str, np.ndarray], rows: np.ndarray, file: str
    ) -> dict[str, np.ndarray]:
        kind = self.src.kind
        tname = TIME_COLUMN[kind]
        t = out[tname]
        if len(t) == 0:
            return out

        # NON_MONOTONIC_TS: earlier than the running maximum of everything before it.
        carry = np.iinfo(np.int64).min if self.prev_t is None else self.prev_t
        run_max = np.maximum.accumulate(np.concatenate([[carry], t]))[:-1]
        nonmono = t < run_max
        self._flag("NON_MONOTONIC_TS", nonmono, rows, file)
        if self.policy["NON_MONOTONIC_TS"] == DROP and nonmono.any():
            keep = ~nonmono
            out = {k: v[keep] for k, v in out.items()}
            rows, t = rows[keep], t[keep]

        # DUPLICATE: bars repeat the open time; quotes/trades repeat the whole row.
        if len(t):
            fields = [tname] if kind == "bars" else [f for f in SCHEMAS[kind].names if f in out]
            cur = np.stack([out[f].astype(np.int64) for f in fields], axis=1)
            prev = np.empty_like(cur)
            prev[1:] = cur[:-1]
            if self.prev_row is not None and len(self.prev_row) == len(fields):
                prev[0] = self.prev_row
                dup = np.all(cur == prev, axis=1)
            else:
                dup = np.all(cur == prev, axis=1)
                dup[0] = False
            self._flag("DUPLICATE", dup, rows, file)
            if self.policy["DUPLICATE"] == DROP and dup.any():
                keep = ~dup
                out = {k: v[keep] for k, v in out.items()}
                rows, t = rows[keep], t[keep]
                cur = cur[keep]
            if len(t):
                self.prev_row = tuple(int(x) for x in cur[-1])

        if len(t):
            self.prev_t = int(max(self.prev_t if self.prev_t is not None else t.max(), t.max()))
        return out


# --- Driver -------------------------------------------------------------------------------------


@dataclass
class IngestResult:
    report: Report
    manifest: dict[str, Any] | None  # None when validating only
    path: Path | None


def dataset_id(registry: list[dict[str, Any]], files: list[dict[str, Any]]) -> str:
    """SHA-256 over the logical content: schema version, registry and every file's content hash."""
    keyed = [
        {
            k: f[k]
            for k in ("kind", "instrument_id", "rows", "content_sha256", "interval_ns")
            if k in f
        }
        for f in files
    ]
    return hashlib.sha256(
        _canonical({"schema_version": SCHEMA_VERSION, "instruments": registry, "files": keyed})
    ).hexdigest()


def ingest(
    spec: DatasetSpec | dict[str, Any] | str | Path,
    out: str | Path | None = None,
    *,
    root: str | Path | None = None,
    force: bool = False,
) -> IngestResult:
    """Ingest a dataset. With `out=None`, only validates. Raises DatasetRejected (nothing written)
    if any finding's policy is reject."""
    if isinstance(spec, dict):
        spec = DatasetSpec.from_dict(spec, Path(root or "."))
    elif not isinstance(spec, DatasetSpec):
        spec = DatasetSpec.load(spec, root)

    out_path = Path(out) if out is not None else None
    tmp: Path | None = None
    if out_path is not None:
        if out_path.exists() and not force:
            raise FileExistsError(f"{out_path} exists (use force=True to replace it)")
        tmp = out_path.with_name(f".{out_path.name}.tmp-{os.getpid()}")
        if tmp.exists():
            shutil.rmtree(tmp)
        tmp.mkdir(parents=True)

    report = Report()
    files: list[dict[str, Any]] = []
    sources_meta: list[dict[str, Any]] = []
    try:
        order = {k: i for i, k in enumerate(KINDS)}
        ordered = sorted(
            spec.sources,
            key=lambda s: (spec.registry.by_symbol(s.symbol).instrument_id, order[s.kind]),
        )
        for src in ordered:
            inst = spec.registry.by_symbol(src.symbol)
            policy = resolve_policy(src.kind, spec.policies.get(src.kind))
            rel = f"{src.kind}/{src.symbol}.parquet"
            target = None
            if tmp is not None:
                (tmp / src.kind).mkdir(exist_ok=True)
                target = tmp / rel
            writer = _Writer(target, src.kind, spec.chunk_rows)
            job = _SourceIngest(src, inst, policy, report)
            file_meta = []
            for path in src.paths:
                job.start_file()
                before = job.stats.rows_read
                for raw in iter_source(path, src.format, src.columns, spec.chunk_rows):
                    normalised = job.process(raw)
                    if normalised is not None:
                        writer.append(normalised)
                file_meta.append(
                    {
                        "name": path.name,
                        "sha256": _file_sha256(path),
                        "bytes": path.stat().st_size,
                        "rows": job.stats.rows_read - before,
                        "ts_unit": job.file_unit,
                    }
                )
            writer.close()
            sources_meta.append(
                {"kind": src.kind, "symbol": src.symbol, "format": src.format, "files": file_meta}
            )
            entry: dict[str, Any] = {
                "kind": src.kind,
                "symbol": src.symbol,
                "instrument_id": inst.instrument_id,
                "path": rel,
                "rows": writer.rows,
                "ts_min_ns": writer.ts_min,
                "ts_max_ns": writer.ts_max,
                "content_sha256": writer.content_sha256(),
            }
            if src.kind == "bars":
                entry["interval_ns"] = src.interval_ns
            files.append({**entry, "_emit_max": writer.emit_max})

        if report.rejected:
            raise DatasetRejected(report)

        registry = spec.registry.to_dicts()
        emit_max = [f.pop("_emit_max") for f in files]
        starts = [f["ts_min_ns"] for f in files if f["ts_min_ns"] is not None]
        ends = [e for e in emit_max if e is not None]
        manifest: dict[str, Any] = {
            "format": "finforge-dataset",
            "schema_version": SCHEMA_VERSION,
            "id": dataset_id(registry, files),
            "name": spec.name,
            "instruments": registry,
            "files": files,
            "time_range_ns": {
                "first": min(starts) if starts else None,
                "last_emit": max(ends) if ends else None,
            },
            "chunk_rows": spec.chunk_rows,
            "sources": sources_meta,
            "validation": {**report.to_dict(with_samples=False), "report": "validation.json"},
            "notes": spec.notes,
        }
        if tmp is None or out_path is None:
            return IngestResult(report=report, manifest=manifest, path=None)

        (tmp / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=False) + "\n")
        (tmp / "validation.json").write_text(json.dumps(report.to_dict(), indent=2) + "\n")
        if out_path.exists():
            shutil.rmtree(out_path)
        tmp.rename(out_path)
        tmp = None
        return IngestResult(report=report, manifest=manifest, path=out_path)
    finally:
        if tmp is not None and tmp.exists():
            shutil.rmtree(tmp)


__all__ = [
    "DEFAULT_CHUNK_ROWS",
    "DatasetRejected",
    "DatasetSpec",
    "IngestResult",
    "SourceSpec",
    "dataset_id",
    "ingest",
    "parse_interval",
]
