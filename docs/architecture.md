# Architecture

How FinForge is put together, and why. The rules every component obeys (numbers, time, ordering,
fills, accounting, metrics) are in [`contracts.md`](contracts.md). This document is the map.

Priorities, in order: **correctness → determinism → reproducibility → clarity → performance →
features.** Most decisions below follow from that order.

## 1. The big picture

```text
 raw CSV / zip / Parquet ─► finforge.data: ingest → validate → normalise ─► dataset dir
                            (exact text→integer, structured report, content-hash id)  (Parquet + manifest)
                                                                                    │ numpy columns,
                                                                                    ▼ chunk by chunk
 ┌──────────────────────────────── C++ engine (deterministic) ─────────────────────────────────┐
 │  Replay (k-way merge) ─► EventQueue (ts, class, seq) ─► Simulator ─► ExecutionModel          │
 │                                                          │   │      (Candle | Quote | Book)   │
 │                         Strategy callbacks ◄── Context ◄─┘   ▼ fills                          │
 │                         (Python, via pybind11)          Accounts / Ledger ─► Journal (SHA-256)│
 └──────────────────────────────────────────────────────────────────────────────────────────────┘
                                                                                    │ arrays at the end
                                                                                    ▼
 finforge.backtest: run dir (manifest.json, orders/fills/equity/exposure Parquet) ─► finforge.analytics
                                                                                     report.json, compare, sweep
 finforge.server: FastAPI · Postgres (metadata + job queue, SKIP LOCKED) · worker (one child process per run) · SSE
 frontend/: Next.js dashboard, talks to the API only
```

Two modes share the core:
- **Mode A, historical backtesting.** Bars or quotes drive a Candle or Quote execution model.
  Results are "approximate execution under documented assumptions".
- **Mode B, exchange simulation.** A real price-time-priority order book, driven by scripted or
  synthetic order flow (`finforge-sim`). This is "exact matching under our rules".

## 2. Who owns what

| Concern | Where | Language | Why there |
|---|---|---|---|
| Strong types, fixed-point money | `engine/include/finforge/types.hpp`, `instrument.hpp` | C++ | Exact integer arithmetic everywhere it matters (D2) |
| Order book, matching | `order_book.hpp`, `matching_engine.hpp` | C++ | Hot path; tested against a naive reference book |
| Event queue, clock, simulator | `event_queue.hpp`, `simulator.hpp` | C++ | Determinism: one total order of events |
| Execution models | `execution_model.hpp` (Book), `mode_a.hpp` (Candle, Quote) | C++ | Fills and accounting in the same loop |
| Pre-trade checks, ledger, equity | `accounts.hpp` | C++ | Integer accounting, reconciled after every run |
| Journal and fingerprint | `journal.hpp`, `sha256.hpp` | C++ | Every event recorded; its SHA-256 is the run fingerprint |
| Market data replay | `replay.hpp` | C++ | k-way merge of chunked columns, bounded memory |
| Mode B CLI and synthetic flow | `engine/apps/finforge_sim.cpp`, `synthetic.hpp`, `rng.hpp` | C++ | Standalone demo with its own portable PRNG |
| Bindings | `python/bindings/` (`finforge._engine`) | C++ / pybind11 | Thin: arrays in, callbacks, arrays out |
| Data pipeline | `finforge.data` (ingest, validate, fixedpoint, sources, dataset, demo) | Python (pyarrow) | Columnar tooling; exactness via text parsing |
| Strategy SDK | `finforge.sdk`, `finforge.strategies` | Python | User-facing flexibility |
| Backtest orchestration | `finforge.backtest`, `finforge.config`, `finforge.cli` | Python | Config, artefacts, manifest |
| Analytics | `finforge.analytics` | Python (numpy) | Post-hoc, not hot path |
| Service | `finforge.server` (api, db, queue, worker, service, storage) | Python (FastAPI, SQLAlchemy) | Standard web stack |
| Dashboard | `frontend/` | TypeScript (Next.js) | Consumes the API only, no simulation logic |

The C++ engine has no Python dependency and builds and tests on its own
(`cmake --workflow --preset dev`).

## 3. Key decisions

Each one is stated with its consequence; the full rationale is in the project plan.

- **D1: C++ owns the loop, execution and accounting; Python owns everything around it.**
  Strategies are Python callbacks that the C++ loop calls. One backtest runs per process, so
  parallel sweeps use processes, not threads.
- **D2: Fixed point everywhere in the engine.**
  - Prices are `int64` ticks, quantities `int64` lots, money `int64` at 10⁻⁸.
  - Products go through `__int128` with checked narrowing.
  - Doubles appear only at the edges (ingest in, analytics out); CI greps the engine for floats
    (NUM-1).
- **D3: Determinism is tested, not hoped for.** Same inputs give a byte-identical journal, hence an
  identical SHA-256. CI pins a 1 M-event fingerprint, and the same config run twice gives identical
  artefact files.
- **D4: One `ExecutionModel` interface.** Book (Mode B), Candle and Quote (Mode A). Every result
  records the model that produced it.
- **D5: Binance public data** (spot klines, USD-M futures bookTicker) for real data, and synthetic
  generators for tests and demos.
- **D6: Versions are content hashes.** Each run's manifest records:
  - the dataset id (hash of its normalised content);
  - the strategy hash (source, name, parameters);
  - the engine version (semver, git SHA, build type).
- **D7: Storage split.** Postgres holds metadata, statuses, metrics and the job queue
  (`FOR UPDATE SKIP LOCKED`, leases, heartbeats). Parquet holds bulk series. Progress goes over
  SSE. There is no Redis or Celery.
- **D8: The API runs built-in strategies only**, with parameters validated against a JSON Schema.
  Custom strategies run locally through the SDK. No user code runs on the server.
- **D9–D10: Toolchain.** Linux (WSL2) first, GCC 13+ or Clang 17+, CMake presets, dependencies
  through `FetchContent`, the Python package built with scikit-build-core.
- **D11: Simplest structure first, optimise only with a benchmark.**
  - The book is a `std::map` of FIFO price levels plus an id index.
  - Phase 9 measured it at 3.4–11 M ops/s, which beats every target, so the flat-ladder and pool
    variants were not needed (see [`benchmarks.md`](benchmarks.md)).

## 4. The boundaries that matter

**Time and look-ahead.**
- Timestamps are `int64` ns UTC. A bar becomes visible at its *close*.
- Orders arrive at least 1 ns after they are sent, so a strategy can never trade at a price it has
  just seen.
- `ctx.history()` returns read-only views that end at `now`.

**Event order.** The queue key is `(ts, class, instrument, seq)`. Same-timestamp market data from
several instruments and feeds is merged by `(emit time, instrument, kind, row)` (EVT-1, EVT-2).
There is never a tie left to chance.

**The language boundary.**
- Data goes to C++ as numpy arrays without copying, chunk by chunk.
- The engine calls back into Python with the GIL held.
- The Python `Context` holds only a *weak* reference to the engine. The engine holds the handler,
  so a strong reference would form a cycle through a C++ member that Python's GC cannot collect.
- Python exceptions in a strategy fail the run, with the traceback in the manifest.

**The trust boundary (server).**
- Uploads: size-capped (refused on `Content-Length` first), type-sniffed, stored under names the
  server generates.
- Specs refer to uploads by id, never by path.
- Strategies are built-in only and their parameters are schema-checked.
- No authentication: the stack listens on 127.0.0.1 ([`server.md`](server.md) §5).

**Jobs.**
- A worker claims a job with `SKIP LOCKED`, runs the backtest in a spawned child process, and
  renews its lease with heartbeats.
- An expired lease is reaped: retried, or failed after the last attempt.
- Every result write is guarded by the lease, so a late or duplicate write is a no-op.
- Cancel sets a flag that reaches the engine's cancel check, and the run stops between two events.

## 5. Repository layout

| Path | What |
|---|---|
| `engine/` | C++20 core library (`include/finforge`, `src`), tests (`tests`, incl. the reference book), benchmarks (`bench`), `finforge-sim` (`apps`) |
| `python/bindings/` | pybind11 module `finforge._engine` |
| `python/finforge/` | `data`, `sdk`, `strategies`, `config`, `backtest`, `analytics`, `cli`, `server` |
| `python/tests/` | pytest (data, SDK, backtests, analytics, server against Postgres) |
| `frontend/` | Next.js dashboard; typed client generated from the OpenAPI document; Playwright Demo 1 |
| `scripts/` | data download, fixtures, benchmarks (`bench.py`), e2e stack, CI checks |
| `examples/` | backtest and sweep configs |
| `datasets/` | committed fixtures and specs only; downloaded data stays local |
| `Dockerfile`, `frontend/Dockerfile`, `docker-compose.yml` | the one-command stack |
| `docs/` | how the system *is* (this file, contracts, data, execution, SDK, analytics, server, dashboard, benchmarks) |

Differences from the original plan's layout:
- The service lives in `python/finforge/server/` (one package, one version), not in a separate
  `backend/`.
- The SDK is `finforge.sdk` (one module), not a `strategy/` package.
- Backtest orchestration is `finforge.backtest` (one module).
