"""Ingestion, validation findings per fixture, manifest stability (PLAN Phase 3 exit criteria)."""

import json
import shutil
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from finforge.data import Dataset, DatasetRejected, ingest
from finforge.data.__main__ import main as cli

FIXTURES = Path(__file__).resolve().parents[2] / "datasets" / "fixtures"
T0_NS = 1_709_251_200_000 * 1_000_000  # 2024-03-01T00:00:00Z
MINUTE_NS = 60 * 10**9

TRADE_COLUMNS = {"ts": "time", "price": "price", "qty": "size", "aggressor_side": "side"}
JSON_COLUMNS = {"ts": "t", "bid": "b", "bid_qty": "bq", "ask": "a", "ask_qty": "aq"}


def one(fmt: str, path: str, symbol: str = "AAA", **extra) -> dict:
    src = {"format": fmt, "symbol": symbol, "paths": [path], **extra}
    if fmt == "binance_klines":
        src.setdefault("interval", "1m")
    return {"name": "t", "registry": "registry.json", "sources": [src]}


def sample_spec(**extra) -> dict:
    return {
        "name": "fixture-sample",
        "registry": "registry.json",
        "sources": [
            {
                "format": "binance_klines",
                "symbol": "AAA",
                "interval": "1m",
                "paths": ["klines_AAA_ms.csv"],
            },
            {
                "format": "binance_klines",
                "symbol": "BBB",
                "interval": "1m",
                "paths": ["klines_BBB_ms.csv"],
            },
            {"format": "binance_bookticker", "symbol": "AAA", "paths": ["bookticker_AAA.csv"]},
            {
                "format": "generic",
                "kind": "quotes",
                "symbol": "BBB",
                "paths": ["quotes_BBB.json"],
                "columns": JSON_COLUMNS,
            },
        ],
        **extra,
    }


def findings(spec: dict) -> dict[str, int]:
    try:
        return ingest(spec, None, root=FIXTURES).report.counts()
    except DatasetRejected as e:
        return e.report.counts()


# --- Each malformed fixture produces the expected findings (exit criterion 2) -------------------

EXPECTED = [
    ("klines_AAA_ms.csv", "binance_klines", {}, {}),
    ("klines_BBB_ms.csv", "binance_klines", {"symbol": "BBB"}, {}),
    ("klines_AAA_us.csv", "binance_klines", {}, {}),
    ("bookticker_AAA.csv", "binance_bookticker", {}, {}),
    ("klines_ambiguous.csv", "binance_klines", {}, {"TS_UNIT_AMBIGUOUS": 30}),
    ("klines_ohlc.csv", "binance_klines", {}, {"OHLC_INCONSISTENT": 2}),
    ("klines_sequence.csv", "binance_klines", {}, {"DUPLICATE": 1, "NON_MONOTONIC_TS": 1}),
    ("klines_gap.csv", "binance_klines", {}, {"GAP": 1}),
    ("klines_grid.csv", "binance_klines", {}, {"OFF_TICK": 1, "OFF_LOT": 1}),
    (
        "klines_values.csv",
        "binance_klines",
        {},
        {
            "MALFORMED_ROW": 1,
            "MISSING_VALUE": 1,
            "BAD_NUMBER": 1,
            "NEGATIVE_QTY": 1,
            "NON_POSITIVE_PRICE": 1,
            "BAD_CLOSE_TIME": 1,
        },
    ),
    (
        "bookticker_dirty.csv",
        "binance_bookticker",
        {},
        {"CROSSED_QUOTE": 2, "DUPLICATE": 1, "NON_MONOTONIC_TS": 1},
    ),
    (
        "trades_BBB.csv",
        "generic",
        {"symbol": "BBB", "kind": "trades", "columns": TRADE_COLUMNS},
        {"BAD_VALUE": 1},
    ),
    (
        "quotes_BBB.json",
        "generic",
        {"symbol": "BBB", "kind": "quotes", "columns": JSON_COLUMNS},
        {},
    ),
]


@pytest.mark.parametrize(("path", "fmt", "extra", "want"), EXPECTED, ids=[e[0] for e in EXPECTED])
def test_fixture_findings(path: str, fmt: str, extra: dict, want: dict[str, int]) -> None:
    assert findings(one(fmt, path, **extra)) == want


def test_finding_rows_and_actions() -> None:
    with pytest.raises(DatasetRejected) as e:
        ingest(one("binance_klines", "klines_values.csv"), None, root=FIXTURES)
    rep = e.value.report.to_dict()
    assert rep["status"] == "rejected"
    rows = {f["code"]: [s["row"] for s in f["samples"]] for f in rep["findings"]}
    assert rows == {
        "MALFORMED_ROW": [31],
        "MISSING_VALUE": [2],
        "BAD_NUMBER": [4],
        "NON_POSITIVE_PRICE": [8],
        "NEGATIVE_QTY": [6],
        "BAD_CLOSE_TIME": [10],
    }
    assert all(f["action"] == "reject" for f in rep["findings"])

    rep = ingest(one("binance_klines", "klines_gap.csv"), None, root=FIXTURES).report.to_dict()
    assert rep["status"] == "flagged"
    (gap,) = rep["findings"]
    assert (gap["action"], gap["missing_intervals"], gap["samples"][0]["row"]) == ("flag", 3, 16)


def test_drop_policy_removes_rows_and_counts_them(tmp_path: Path) -> None:
    spec = one("binance_bookticker", "bookticker_dirty.csv")
    spec["policies"] = {"quotes": {"NON_MONOTONIC_TS": "drop", "DUPLICATE": "drop"}}
    res = ingest(spec, tmp_path / "ds", root=FIXTURES)
    rep = res.report.to_dict()
    assert rep["status"] == "flagged"
    assert (rep["rows_read"], rep["rows_written"], rep["rows_dropped"]) == (41, 37, 4)
    ts = Dataset(tmp_path / "ds").read("quotes", "AAA").column("ts").to_pylist()
    assert ts == sorted(ts)


def test_policy_must_be_allowed_for_the_code() -> None:
    spec = one("binance_klines", "klines_grid.csv")
    spec["policies"] = {"bars": {"OFF_TICK": "flag"}}  # no representable value: reject/drop only
    with pytest.raises(ValueError, match="OFF_TICK"):
        ingest(spec, None, root=FIXTURES)


def test_rejected_ingest_writes_nothing(tmp_path: Path) -> None:
    with pytest.raises(DatasetRejected):
        ingest(one("binance_klines", "klines_ohlc.csv"), tmp_path / "ds", root=FIXTURES)
    assert list(tmp_path.iterdir()) == []


# --- Exact conversions (TIME-1, TIME-2, TIME-6) -------------------------------------------------


def test_timestamps_convert_exactly_in_both_units(tmp_path: Path) -> None:
    spec = {
        "name": "units",
        "registry": "registry.json",
        "sources": [
            {
                "format": "binance_klines",
                "symbol": "AAA",
                "interval": "1m",
                "paths": ["klines_AAA_ms.csv", "klines_AAA_us.csv"],
            },
        ],
    }
    res = ingest(spec, tmp_path / "ds", root=FIXTURES)
    files = res.manifest["sources"][0]["files"]
    assert [(f["name"], f["ts_unit"]) for f in files] == [
        ("klines_AAA_ms.csv", "ms"),
        ("klines_AAA_us.csv", "us"),
    ]
    t = Dataset(tmp_path / "ds").read("bars", "AAA")
    ts_open = t.column("ts_open").to_pylist()
    ts_close = t.column("ts_close").to_pylist()
    assert ts_open[0] == T0_NS  # 1709251200000 ms
    assert ts_open[300] == 1_735_689_600_000_000 * 1000  # 1735689600000000 µs
    assert all(c - o == MINUTE_NS for o, c in zip(ts_open, ts_close, strict=True))  # TIME-2
    # The first bar: 100.00 / 100.00 / 99.95 / 99.97, volume 0.965 (ticks 0.01, lots 0.001).
    first = {k: t.column(k)[0].as_py() for k in ("open", "high", "low", "close", "volume")}
    assert first == {"open": 10000, "high": 10000, "low": 9995, "close": 9997, "volume": 965}


def test_explicit_unit_mismatch_is_ambiguous() -> None:
    spec = one(
        "generic",
        "quotes_BBB.json",
        symbol="BBB",
        kind="quotes",
        columns=JSON_COLUMNS,
        ts_unit="ms",  # the file is in µs
    )
    assert findings(spec) == {"TS_UNIT_AMBIGUOUS": 10}


def test_generic_parquet_with_floats_and_timestamp_type(tmp_path: Path) -> None:
    table = pa.table(
        {
            "time": pa.array([T0_NS, T0_NS + MINUTE_NS], pa.timestamp("ns", tz="UTC")),
            "o": [2500.5, 2501.0],
            "h": [2502.0, 2503.5],
            "l": [2499.5, 2500.0],
            "c": [2501.0, 2502.0],
            "v": [3, 4],
        }
    )
    pq.write_table(table, tmp_path / "bars.parquet")
    shutil.copy(FIXTURES / "registry.json", tmp_path / "registry.json")
    spec = one(
        "generic",
        "bars.parquet",
        symbol="BBB",
        kind="bars",
        interval="1m",
        columns={
            "ts_open": "time",
            "open": "o",
            "high": "h",
            "low": "l",
            "close": "c",
            "volume": "v",
        },
    )
    res = ingest(spec, tmp_path / "ds", root=tmp_path)
    assert res.report.status == "clean"
    t = Dataset(tmp_path / "ds").read("bars", "BBB")
    assert t.column("open").to_pylist() == [5001, 5002]  # ticks of 0.5
    assert t.column("ts_open").to_pylist() == [T0_NS, T0_NS + MINUTE_NS]


# --- Manifest (task 5) and stability (exit criterion 1) -----------------------------------------


def test_manifest_is_stable_and_independent_of_chunking(tmp_path: Path) -> None:
    a = ingest(sample_spec(), tmp_path / "a", root=FIXTURES)
    b = ingest(sample_spec(), tmp_path / "b", root=FIXTURES)
    c = ingest(sample_spec(chunk_rows=7), tmp_path / "c", root=FIXTURES)
    ma = (tmp_path / "a" / "manifest.json").read_bytes()
    assert ma == (tmp_path / "b" / "manifest.json").read_bytes()
    assert a.manifest["id"] == b.manifest["id"] == c.manifest["id"]
    assert (
        len(
            pq.ParquetFile(tmp_path / "c" / "bars" / "AAA.parquet").metadata.to_dict()["row_groups"]
        )
        == 43
    )

    m = json.loads(ma)
    assert m["validation"]["status"] == "clean"
    assert [(f["kind"], f["symbol"], f["rows"]) for f in m["files"]] == [
        ("bars", "AAA", 300),
        ("quotes", "AAA", 300),
        ("bars", "BBB", 300),
        ("quotes", "BBB", 10),
    ]
    assert [i["symbol"] for i in m["instruments"]] == ["AAA", "BBB"]  # INS-2
    assert m["time_range_ns"]["first"] == T0_NS
    for f in m["sources"]:
        assert all(len(x["sha256"]) == 64 for x in f["files"])
    Dataset(tmp_path / "a").verify()
    Dataset(tmp_path / "c").verify()


def test_id_changes_with_content_and_verify_catches_tampering(tmp_path: Path) -> None:
    a = ingest(sample_spec(), tmp_path / "a", root=FIXTURES)
    spec = sample_spec()
    spec["sources"] = spec["sources"][:1]
    b = ingest(spec, tmp_path / "b", root=FIXTURES)
    assert a.manifest["id"] != b.manifest["id"]

    path = tmp_path / "a" / "bars" / "AAA.parquet"
    t = pq.read_table(path)
    vol = pa.array([v + 1 for v in t.column("volume").to_pylist()], pa.int64())
    pq.write_table(
        t.set_column(t.schema.get_field_index("volume"), t.schema.field("volume"), vol), path
    )
    with pytest.raises(ValueError, match="content hash"):
        Dataset(tmp_path / "a").verify()


def test_existing_output_needs_force(tmp_path: Path) -> None:
    ingest(sample_spec(), tmp_path / "a", root=FIXTURES)
    with pytest.raises(FileExistsError):
        ingest(sample_spec(), tmp_path / "a", root=FIXTURES)
    ingest(sample_spec(), tmp_path / "a", root=FIXTURES, force=True)


def test_cli_ingest_validate_info_verify(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    spec_path = tmp_path / "spec.json"
    spec_path.write_text(json.dumps(sample_spec()))
    assert cli(["validate", str(spec_path), "--root", str(FIXTURES)]) == 0
    assert (
        cli(["ingest", str(spec_path), "--root", str(FIXTURES), "--out", str(tmp_path / "ds")]) == 0
    )
    assert cli(["info", str(tmp_path / "ds")]) == 0
    assert cli(["verify", str(tmp_path / "ds")]) == 0
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps(one("binance_klines", "klines_ohlc.csv")))
    assert cli(["validate", str(bad), "--root", str(FIXTURES)]) == 2
    out = capsys.readouterr().out
    assert "OHLC_INCONSISTENT" in out and "rejected" in out
