# FinForge

A market simulation and research platform: a C++20 engine (order book, matching, event-driven
simulation, accounting) driven by Python strategies, with analytics, an API and a web dashboard.

Priorities, in order: **correctness → determinism → reproducibility → clarity → performance → features.**

> Status: Phase 5 (Python strategy SDK and end-to-end backtest). [`docs/contracts.md`](docs/contracts.md)
> has the rules the engine obeys; [`docs/sdk.md`](docs/sdk.md) how to write and run a strategy;
> [`docs/data.md`](docs/data.md) describes datasets and replay;
> [`docs/execution.md`](docs/execution.md) the Candle/Quote/Book models and exact accounting;
> [`docs/benchmarks.md`](docs/benchmarks.md) has the performance baseline.

## Requirements

Linux (WSL2 Ubuntu 24.04 is the primary dev environment), GCC 13+ or Clang 17+, CMake ≥ 3.28,
Ninja, and [`uv`](https://docs.astral.sh/uv/) with Python 3.12.

## Quickstart

```bash
# C++ engine: Debug build with ASan/UBSan, then tests
cmake --workflow --preset dev        # or: cmake --preset dev && cmake --build --preset dev && ctest --preset dev

# Release build
cmake --workflow --preset release

# Python package (builds the C++ extension) and tests
uv venv --python 3.12
uv pip install -e . --group dev
source .venv/bin/activate && pytest
```

## Order book demo (`finforge-sim`)

Runs an order script against one instrument and prints every event and the final book:

```bash
cmake --workflow --preset release
build/release/engine/finforge-sim engine/apps/demo/orders.csv
```

Script lines: `ts,owner,side,type,tif,price,qty` (e.g. `1,7,BUY,LIMIT,GTC,100,5`; MARKET price
empty), `cancel,id`, or `amend,id,new_qty,new_price`. The expected output is kept as a golden file,
`engine/apps/demo/expected.txt`, and checked by `ctest`.

### Synthetic multi-agent simulation (Mode B)

```bash
build/release/engine/finforge-sim --synthetic --seed 42 --events 1000000 [--agents 8] \
    [--latency-ns 1000] [--journal run.ffj]
```

Poisson-arrival agents trade on one book through the discrete-event simulator. The output ends
with the journal's SHA-256, the run fingerprint: the same seed gives the same hash in every build
(`docs/contracts.md` §10).

## Backtest a Python strategy

```bash
finforge backtest examples/ma_cross.yaml      # needs the sample dataset (see Data below)
```

A run writes `manifest.json` (config, dataset id, strategy hash, engine version, journal hash,
status) plus `orders.parquet`, `fills.parquet` and `equity.parquet`. The same config always gives
the same journal hash and byte-identical files. See [`docs/sdk.md`](docs/sdk.md).

## Benchmarks

```bash
cmake --workflow --preset release
build/release/engine/finforge_bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

## Data

Raw market data is not committed. `scripts/fetch_binance.py` downloads Binance public data (PLAN.md
D5) and verifies every file's checksum. `finforge.data` turns it into a validated, versioned dataset
and replays it through the engine:

```bash
python scripts/fetch_binance.py --bars   # spot 1m klines, BTCUSDT + ETHUSDT, 2023–2024 (~95 MB)
python -m finforge.data ingest datasets/specs/binance-spot-1m.json --out datasets/normalized/binance-spot-1m
python -m finforge.data replay datasets/normalized/binance-spot-1m
```

Prices and quantities are converted from text to integer ticks and lots exactly. Validation returns
a structured report (reject / drop / flag per problem), and the dataset id is a content hash. See
[`docs/data.md`](docs/data.md).

## Layout

| Path | What |
|---|---|
| `engine/` | C++20 core library, no Python dependency |
| `engine/apps/` | `finforge-sim` CLI and its demo script |
| `engine/bench/` | Google Benchmark workloads |
| `python/bindings/` | pybind11 module `finforge._engine` |
| `python/finforge/` | Python package: `sdk`, `backtest`, `config`, `strategies`, `data`, `cli` |
| `examples/` | Backtest configs for the reference strategies |
| `datasets/fixtures/` | Small synthetic test data, including malformed files |
| `datasets/specs/` | Dataset specs for the PLAN D5 data |
| `scripts/` | Data download, fixture generator, CI checks |
| `docs/` | Product docs: how the system **is** |

## License

MIT
