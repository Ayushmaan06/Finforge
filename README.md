# FinForge

A market simulation and research platform: a C++20 engine (order book, matching, event-driven
simulation, accounting) driven by Python strategies, with analytics, an API and a web dashboard.

Priorities, in order: **correctness → determinism → reproducibility → clarity → performance → features.**

> Status: **v1.0.0**. See [`CHANGELOG.md`](CHANGELOG.md).

| Read | For |
|---|---|
| [`docs/tutorial.md`](docs/tutorial.md) | your first strategy, step by step |
| [`docs/architecture.md`](docs/architecture.md) | how the pieces fit and why |
| [`docs/contracts.md`](docs/contracts.md) | the rules the engine obeys (numbers, time, fills, accounting, metrics) |
| [`docs/sdk.md`](docs/sdk.md) · [`docs/analytics.md`](docs/analytics.md) | strategy SDK reference · reports, comparisons, sweeps |
| [`docs/data.md`](docs/data.md) · [`docs/execution.md`](docs/execution.md) | datasets and replay · the Candle/Quote/Book models |
| [`docs/server.md`](docs/server.md) · [`docs/dashboard.md`](docs/dashboard.md) | HTTP API and workers · the web UI |
| [`docs/benchmarks.md`](docs/benchmarks.md) | measured performance and how to reproduce it |

## Try it: the whole stack in one command

Needs only Docker (with Compose v2):

```bash
git clone https://github.com/Ayushmaan06/Finforge.git && cd Finforge
docker compose up --build        # first build: a few minutes (compiles the C++ engine)
```

Open **http://localhost:3000**. The stack has PostgreSQL, the API (http://localhost:8000/docs), a
worker, the dashboard, and a one-year synthetic 1-minute dataset called `demo-365d-1m`. This is
**Demo 1**:
1. **New backtest:** pick `demo-365d-1m` and `ma_cross`.
2. Set the parameters, capital, fees, slippage and execution model, then start. Watch progress live.
3. Inspect equity against buy-and-hold, drawdown, returns, trades on the price chart, and the risk
   and execution statistics.
4. Start a second parameter set, then **Compare** them, with an in-sample / out-of-sample split.

Stop with `docker compose down` (add `-v` to delete the database and stored runs). Everything
listens on 127.0.0.1 only; there is no authentication.

The image has no `.git`, so runs record the engine version as `1.0.0+unknown`. To record the
commit as well (PLAN D6), build with `GIT_SHA=$(git rev-parse --short=12 HEAD) docker compose up
--build`.

## Requirements (development)

Linux (WSL2 Ubuntu 24.04 is the primary dev environment), GCC 13+ or Clang 17+, CMake ≥ 3.28,
Ninja, and [`uv`](https://docs.astral.sh/uv/) with Python 3.12. The dashboard needs Node.js
(`frontend/.nvmrc`).

## Quickstart (development)

```bash
# C++ engine: Debug build with ASan/UBSan, then tests
cmake --workflow --preset dev        # or: cmake --preset dev && cmake --build --preset dev && ctest --preset dev

# Release build
cmake --workflow --preset release

# Python package (builds the C++ extension) and tests
uv venv --python 3.12
uv pip install -e . --group dev
source .venv/bin/activate && pytest

# Also the API tests: the server extra and a PostgreSQL (each test creates its own database)
uv pip install -e ".[server]" --group dev && docker compose up -d --wait db
FINFORGE_TEST_DATABASE_URL=postgresql+psycopg://finforge:finforge@localhost:5432/finforge pytest
```

## Demo 2: the order book (`finforge-sim`)

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

New here? [`docs/tutorial.md`](docs/tutorial.md) writes, runs, compares and sweeps a strategy on
synthetic data in about ten minutes.

```bash
finforge backtest examples/ma_cross.yaml      # needs the sample dataset (see Data below)
```

A run writes `manifest.json` (config, dataset id, strategy hash, engine version, journal hash,
status) plus `orders.parquet`, `fills.parquet` and `equity.parquet`. The same config always gives
the same journal hash and byte-identical files. See [`docs/sdk.md`](docs/sdk.md).

Completed runs also get `report.json` (Sharpe, Sortino, drawdown, VaR, FIFO trades, exposure,
buy-and-hold benchmark). Compare runs and sweep parameters with in-sample / out-of-sample splits:

```bash
finforge compare runs/a runs/b
finforge sweep examples/ma_cross_sweep.yaml
```

## Run it as a service

```bash
uv pip install -e ".[server]" && docker compose up -d db
finforge server migrate && finforge server api   # OpenAPI docs at http://localhost:8000/docs
finforge server worker                           # runs queued backtests
```

Upload data, register datasets, queue runs of the built-in strategies, follow progress over SSE,
cancel, and fetch results and comparisons. See [`docs/server.md`](docs/server.md).

> The service has no authentication: it is a local research tool and listens on 127.0.0.1 only.
> Do not expose it to a network without an authenticating proxy in front.

## Dashboard

With Docker, `docker compose up --build` runs it (see "Try it" above). To develop the dashboard
against a local backend instead:

```bash
python scripts/e2e_stack.py --port 8000                 # Postgres, API, worker, one-year demo dataset
cd frontend && npm ci && NEXT_PUBLIC_API_URL=http://127.0.0.1:8000 npm run dev
```

Pick a dataset, configure and start a backtest, watch its progress, inspect equity against
buy-and-hold, drawdown, returns, trades and fills on the price chart, and compare runs with an
in-sample / out-of-sample split. See [`docs/dashboard.md`](docs/dashboard.md).

## Benchmarks

```bash
cmake --workflow --preset release && uv pip install -e .
python scripts/bench.py          # C++ book and engine, Python backtests and ingest, vs PLAN targets
```

Method, numbers and profiling recipes: [`docs/benchmarks.md`](docs/benchmarks.md).

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
| `python/finforge/server/` | FastAPI app, SQLAlchemy models, Alembic migrations, job queue, worker |
| `Dockerfile`, `frontend/Dockerfile`, `docker-compose.yml` | The one-command stack (the `db` service alone is the development database) |
| `frontend/` | Next.js + TypeScript dashboard (typed client generated from OpenAPI, Playwright e2e) |
| `datasets/fixtures/` | Small synthetic test data, including malformed files |
| `datasets/specs/` | Dataset specs for the PLAN D5 data |
| `scripts/` | Data download, fixture generator, benchmarks, e2e stack, CI checks |
| `docs/` | Product docs: how the system **is** |

## License

MIT
