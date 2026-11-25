"""Reading a normalised dataset and replaying it through the engine (PLAN Phase 3 tasks 7-8).

Replay streams each file with ``ParquetFile.iter_batches``: only one batch per feed is in memory at
a time, and each batch's columns go to C++ as numpy views of the Arrow buffers (no copy). The C++
``Replay`` merges the feeds by (emit time, instrument id, feed order, row) (contracts.md §5.6);
feeds are attached in manifest order, which is (instrument id, bars < quotes < trades).
"""

import hashlib
import json
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
import pyarrow.parquet as pq

from finforge._engine import Engine
from finforge.data.ingest import DEFAULT_CHUNK_ROWS, _canonical, dataset_id
from finforge.data.registry import Registry
from finforge.data.schema import REPLAY_COLUMNS, SCHEMA_VERSION, SCHEMAS


class Dataset:
    def __init__(self, path: str | Path):
        self.path = Path(path)
        self.manifest: dict[str, Any] = json.loads((self.path / "manifest.json").read_text())
        if self.manifest.get("format") != "finforge-dataset":
            raise ValueError(f"{self.path}: not a FinForge dataset")
        if self.manifest["schema_version"] != SCHEMA_VERSION:
            raise ValueError(
                f"{self.path}: schema version {self.manifest['schema_version']}, "
                f"this build reads {SCHEMA_VERSION}"
            )
        self.registry = Registry.from_dicts(self.manifest["instruments"])  # INS-2

    @property
    def id(self) -> str:
        return self.manifest["id"]

    @property
    def files(self) -> list[dict[str, Any]]:
        return self.manifest["files"]

    def file(self, kind: str, symbol: str) -> dict[str, Any]:
        for f in self.files:
            if f["kind"] == kind and f["symbol"] == symbol:
                return f
        raise KeyError(f"no {kind} file for {symbol}")

    def read(self, kind: str, symbol: str):
        """The whole file as a pyarrow Table (small datasets / analysis only)."""
        return pq.read_table(self.path / self.file(kind, symbol)["path"])

    def iter_chunks(
        self, entry: dict[str, Any], chunk_rows: int
    ) -> Iterator[tuple[np.ndarray, ...]]:
        """Columns of one file in the order the C++ feed expects, `chunk_rows` rows at a time."""
        cols = list(REPLAY_COLUMNS[entry["kind"]])
        pf = pq.ParquetFile(self.path / entry["path"], pre_buffer=False, buffer_size=1 << 20)
        for batch in pf.iter_batches(batch_size=chunk_rows, columns=cols, use_threads=False):
            yield tuple(batch.column(c).to_numpy(zero_copy_only=True) for c in cols)

    def verify(self) -> None:
        """Recompute every file's content hash and the dataset id; raise ValueError on mismatch."""
        for entry in self.files:
            schema = SCHEMAS[entry["kind"]]
            hashes = {f.name: hashlib.sha256() for f in schema}
            rows = 0
            pf = pq.ParquetFile(self.path / entry["path"])
            if not pf.schema_arrow.equals(schema, check_metadata=False):
                raise ValueError(
                    f"{entry['path']}: schema differs from {entry['kind']} v{SCHEMA_VERSION}"
                )
            for batch in pf.iter_batches(batch_size=DEFAULT_CHUNK_ROWS):
                rows += batch.num_rows
                for f in schema:
                    le = np.dtype(f.type.to_pandas_dtype()).newbyteorder("<")
                    a = batch.column(f.name).to_numpy(zero_copy_only=False)
                    hashes[f.name].update(np.ascontiguousarray(a, dtype=le).tobytes())
            meta = {
                "kind": entry["kind"],
                "schema_version": SCHEMA_VERSION,
                "rows": rows,
                "columns": [[f.name, str(f.type), hashes[f.name].hexdigest()] for f in schema],
            }
            if hashlib.sha256(_canonical(meta)).hexdigest() != entry["content_sha256"]:
                raise ValueError(f"{entry['path']}: content hash mismatch")
        if dataset_id(self.manifest["instruments"], self.files) != self.id:
            raise ValueError("dataset id does not match the manifest content")


@dataclass
class ReplayResult:
    status: str
    counts: dict[str, int]
    journal_hash: str
    recorded: dict[str, np.ndarray] | None


def replay(
    dataset: Dataset | str | Path,
    *,
    chunk_rows: int = DEFAULT_CHUNK_ROWS,
    kinds: tuple[str, ...] = ("bars", "quotes", "trades"),
    record: bool = False,
    latency_ns: int = 1,
) -> ReplayResult:
    """Replay a dataset through the simulator with a no-op strategy."""
    ds = dataset if isinstance(dataset, Dataset) else Dataset(dataset)
    if chunk_rows < 1:
        raise ValueError("chunk_rows must be >= 1")
    engine = Engine(ds.registry.to_dicts(), latency_ns=latency_ns, start_ns=0)
    for entry in ds.files:  # manifest order = (instrument_id, kind) = merge tie-break
        if entry["kind"] in kinds:
            engine.add_feed(
                entry["kind"], entry["instrument_id"], ds.iter_chunks(entry, chunk_rows)
            )
    status = engine.run(record=record)
    return ReplayResult(
        status=status,
        counts=dict(engine.counts()),
        journal_hash=engine.journal_hash(),
        recorded=dict(engine.recorded()) if record else None,
    )
