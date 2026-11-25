"""Replay: merge order, chunking, zero-copy boundary, bounded memory (Phase 3 exit criteria)."""

import json
import subprocess
import sys
import textwrap
from pathlib import Path

import numpy as np
import pyarrow.parquet as pq
import pytest
from finforge._engine import Engine

from finforge.data import SCHEMAS, Dataset, ingest, replay
from finforge.data.ingest import _Writer, dataset_id

FIXTURES = Path(__file__).resolve().parents[2] / "datasets" / "fixtures"
KIND_ORDER = {"bars": 0, "quotes": 1, "trades": 2}
TRADE_COLUMNS = {"ts": "time", "price": "price", "qty": "size", "aggressor_side": "side"}


@pytest.fixture(scope="module")
def fixture_dataset(tmp_path_factory: pytest.TempPathFactory) -> Dataset:
    spec = {
        "name": "replay-fixture",
        "registry": "registry.json",
        "policies": {"trades": {"BAD_VALUE": "flag"}},
        "sources": [
            {
                "format": "binance_klines",
                "symbol": "AAA",
                "interval": "1m",
                "paths": ["klines_AAA_ms.csv"],
            },
            {"format": "binance_bookticker", "symbol": "AAA", "paths": ["bookticker_AAA.csv"]},
            {
                "format": "binance_klines",
                "symbol": "BBB",
                "interval": "1m",
                "paths": ["klines_BBB_ms.csv"],
            },
            {
                "format": "generic",
                "kind": "trades",
                "symbol": "BBB",
                "paths": ["trades_BBB.csv"],
                "columns": TRADE_COLUMNS,
            },
        ],
    }
    out = tmp_path_factory.mktemp("ds") / "replay-fixture"
    ingest(spec, out, root=FIXTURES)
    return Dataset(out)


def expected_order(ds: Dataset) -> list[tuple[int, int, int]]:
    """(emit ts, instrument_id, kind) for every row, sorted by the §5.6 merge key:
    emit time, instrument id, feed (bars < quotes < trades), row."""
    rows = []
    for f in ds.files:
        t = ds.read(f["kind"], f["symbol"])
        emit = t.column("ts_close" if f["kind"] == "bars" else "ts").to_pylist()
        k = KIND_ORDER[f["kind"]]
        rows += [(ts, f["instrument_id"], k, i) for i, ts in enumerate(emit)]
    rows.sort()
    return [(ts, inst, k) for ts, inst, k, _ in rows]


def test_replay_emits_every_row_in_merge_order(fixture_dataset: Dataset) -> None:
    ds = fixture_dataset
    n = sum(f["rows"] for f in ds.files)
    assert n == 300 + 300 + 300 + 50
    r = replay(ds, record=True, chunk_rows=64)
    assert r.status == "completed"
    assert r.counts == {"bars": 600, "quotes": 300, "trades": 50, "events_processed": n}
    rec = r.recorded
    got = list(
        zip(rec["ts"].tolist(), rec["instrument_id"].tolist(), rec["kind"].tolist(), strict=True)
    )
    assert got == expected_order(ds)
    # Both instruments really interleave, and same-time ties across instruments occur.
    assert set(rec["instrument_id"].tolist()) == {0, 1}
    ts = rec["ts"]
    assert np.all(np.diff(ts) >= 0)
    assert (ts[1:] == ts[:-1]).sum() > 100


def test_journal_hash_does_not_depend_on_chunk_size(fixture_dataset: Dataset) -> None:
    hashes = {replay(fixture_dataset, chunk_rows=c).journal_hash for c in (1, 7, 64, 65_536)}
    assert len(hashes) == 1


def test_engine_rejects_wrong_arrays_instead_of_copying() -> None:
    inst = [{"symbol": "A", "quote_currency": "USDT", "tick_size": "0.01", "lot_size": "1"}]

    def run(chunk: tuple) -> None:
        e = Engine(inst)
        e.add_feed("quotes", 0, iter([chunk]))
        e.run()

    ok = np.arange(1, 4, dtype=np.int64)
    run((ok, ok, ok, ok, ok))
    with pytest.raises(TypeError, match="int64"):
        run((ok.astype(np.float64), ok, ok, ok, ok))
    with pytest.raises(TypeError, match="C-contiguous"):
        run((np.arange(1, 7, dtype=np.int64)[::2], ok, ok, ok, ok))
    with pytest.raises(ValueError, match="5 columns"):
        run((ok, ok, ok, ok))
    with pytest.raises(ValueError, match="differ in length"):
        run((ok, ok[:2], ok, ok, ok))
    with pytest.raises(ValueError, match="backwards"):
        run((ok[::-1].copy(), ok, ok, ok, ok))
    with pytest.raises(ValueError, match="unknown instrument"):
        Engine(inst).add_feed("quotes", 1, iter([]))
    with pytest.raises(ValueError, match="kind"):
        Engine(inst).add_feed("candles", 0, iter([]))

    def boom():
        yield (ok, ok, ok, ok, ok)
        raise RuntimeError("source failed")

    e = Engine(inst)
    e.add_feed("quotes", 0, boom())
    with pytest.raises(RuntimeError, match="source failed"):
        e.run()


def test_replay_passes_arrow_buffers_without_copying(fixture_dataset: Dataset) -> None:
    entry = fixture_dataset.file("quotes", "AAA")
    chunk = next(fixture_dataset.iter_chunks(entry, 1000))
    for col in chunk:
        assert not col.flags.owndata  # a view of the Arrow buffer
        assert col.flags.c_contiguous


# --- Bounded memory (exit criterion 4) ----------------------------------------------------------

ROWS_PER_INSTRUMENT = 1_500_000
CHUNK = 65_536


def write_big_dataset(path: Path) -> int:
    """Two instruments of synthetic quotes, written directly in the normalised layout."""
    path.mkdir()
    (path / "quotes").mkdir()
    instruments = [
        {
            "instrument_id": i,
            "symbol": s,
            "asset_class": "spot",
            "quote_currency": "USDT",
            "tick_size": "0.01",
            "lot_size": "0.001",
            "min_qty": 1,
            "contract_multiplier": 1,
        }
        for i, s in enumerate(["AAA", "BBB"])
    ]
    files = []
    for inst in instruments:
        rel = f"quotes/{inst['symbol']}.parquet"
        w = _Writer(path / rel, "quotes", CHUNK)
        rng = np.random.default_rng(inst["instrument_id"])
        t = 1_709_251_200_000_000_000
        for start in range(0, ROWS_PER_INSTRUMENT, 250_000):
            n = min(250_000, ROWS_PER_INSTRUMENT - start)
            ts = t + np.cumsum(rng.integers(0, 2_000_000, n))
            t = int(ts[-1])
            bid = 10_000 + rng.integers(-50, 50, n)
            w.append(
                {
                    "ts": ts.astype(np.int64),
                    "instrument_id": np.full(n, inst["instrument_id"], np.uint32),
                    "bid": bid.astype(np.int64),
                    "ask": (bid + 1).astype(np.int64),
                    "bid_qty": rng.integers(1, 1000, n).astype(np.int64),
                    "ask_qty": rng.integers(1, 1000, n).astype(np.int64),
                }
            )
        w.close()
        files.append(
            {
                "kind": "quotes",
                "symbol": inst["symbol"],
                "instrument_id": inst["instrument_id"],
                "path": rel,
                "rows": w.rows,
                "ts_min_ns": w.ts_min,
                "ts_max_ns": w.ts_max,
                "content_sha256": w.content_sha256(),
            }
        )
    manifest = {
        "format": "finforge-dataset",
        "schema_version": 1,
        "id": dataset_id(instruments, files),
        "name": "big",
        "instruments": instruments,
        "files": files,
        "time_range_ns": {"first": files[0]["ts_min_ns"], "last_emit": None},
    }
    (path / "manifest.json").write_text(json.dumps(manifest))
    return 2 * ROWS_PER_INSTRUMENT


# Peak RSS of the replay process. VmHWM belongs to the process image, unlike ru_maxrss, which on
# Linux carries the parent's peak over fork + exec (pytest's own memory would hide the effect).
MEASURE = textwrap.dedent(
    """
    import json, sys
    import numpy, pyarrow.parquet
    from finforge.data import Dataset, replay

    def hwm_kib():
        for line in open("/proc/self/status"):
            if line.startswith("VmHWM:"):
                return int(line.split()[1])

    ds = Dataset(sys.argv[1])
    # Warm-up: the first Parquet read lazily imports modules (pyarrow pulls in pandas when it is
    # installed), which would otherwise count as replay memory.
    next(ds.iter_chunks(ds.files[0], 16))
    before = hwm_kib()
    r = replay(ds, chunk_rows=int(sys.argv[2]))
    after = hwm_kib()
    print(json.dumps({"growth_kib": after - before, "events": r.counts["events_processed"],
                      "status": r.status}))
    """
)


@pytest.mark.skipif(not Path("/proc/self/status").exists(), reason="needs Linux /proc")
def test_large_dataset_replays_with_bounded_memory(tmp_path: Path) -> None:
    n = write_big_dataset(tmp_path / "big")
    assert pq.ParquetFile(tmp_path / "big" / "quotes" / "AAA.parquet").metadata.num_row_groups > 20
    full_kib = n * len(SCHEMAS["quotes"]) * 8 // 1024  # everything in memory at once: ~140 MiB

    def measure(chunk_rows: int) -> dict:
        out = subprocess.run(
            [sys.executable, "-c", MEASURE, str(tmp_path / "big"), str(chunk_rows)],
            check=True,
            capture_output=True,
            text=True,
        )
        m = json.loads(out.stdout)
        assert m["status"] == "completed"
        assert m["events"] == n
        return m

    chunked = measure(CHUNK)
    whole = measure(10 * n)  # control: one chunk per file, i.e. the whole dataset in memory
    # The control shows the measurement can see the data; streaming stays far below it.
    assert whole["growth_kib"] > full_kib // 2, (whole, full_kib)
    assert chunked["growth_kib"] < full_kib // 2, (chunked, full_kib)
    assert chunked["growth_kib"] < whole["growth_kib"] // 3, (chunked, whole)
    print(
        f"\nreplayed {n} rows; peak RSS growth {chunked['growth_kib'] // 1024} MiB streamed vs "
        f"{whole['growth_kib'] // 1024} MiB whole-file (data: {full_kib // 1024} MiB)"
    )
