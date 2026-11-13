# FinForge

A market simulation and research platform: a C++20 engine (order book, matching, event-driven
simulation, accounting) driven by Python strategies, with analytics, an API and a web dashboard.

Priorities, in order: **correctness → determinism → reproducibility → clarity → performance → features.**

> Status: Phase 0 (foundations and contracts). See [`.planning/PLAN.md`](.planning/PLAN.md) for the
> roadmap and [`docs/contracts.md`](docs/contracts.md) for the rules the engine obeys.

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

## Data

Raw market data is not committed. Download it with `scripts/fetch_binance.sh` (Binance public data,
see PLAN.md D5).

## Layout

| Path | What |
|---|---|
| `engine/` | C++20 core library, no Python dependency |
| `python/bindings/` | pybind11 module `finforge._engine` |
| `python/finforge/` | Python package |
| `docs/` | Product docs: how the system **is** |
| `.planning/` | Plan and learning journal: how it **got that way** |

## License

MIT
