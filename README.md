# FinForge

A market simulation and research platform: a C++20 engine (order book, matching, event-driven
simulation, accounting) driven by Python strategies, with analytics, an API and a web dashboard.

Priorities, in order: **correctness → determinism → reproducibility → clarity → performance → features.**

> Status: Phase 1 (order book and matching engine). [`docs/contracts.md`](docs/contracts.md) has the
> rules the engine obeys; [`docs/benchmarks.md`](docs/benchmarks.md) has the performance baseline.

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

## Benchmarks

```bash
cmake --workflow --preset release
build/release/engine/finforge_bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

## Data

Raw market data is not committed. Download it with `scripts/fetch_binance.sh` (Binance public data,
see PLAN.md D5).

## Layout

| Path | What |
|---|---|
| `engine/` | C++20 core library, no Python dependency |
| `engine/apps/` | `finforge-sim` CLI and its demo script |
| `engine/bench/` | Google Benchmark workloads |
| `python/bindings/` | pybind11 module `finforge._engine` |
| `python/finforge/` | Python package |
| `docs/` | Product docs: how the system **is** |

## License

MIT
