# Changelog

## 1.0.0 (2026-11-26)

The first release. It was built in ten phases. Each phase left working, tested software, and CI was
green throughout.

### Phase 0: Foundations and contracts
- CMake presets (dev with ASan/UBSan, release), GoogleTest, pybind11 and scikit-build-core.
- GitHub Actions: release, gcc and clang sanitizer builds, pytest, clang-format, ruff.
- `docs/contracts.md`: fixed-point numbers, time, instruments, order lifecycle, matching, event
  order, execution models, accounting and metrics. Every rule has an ID and a test.

### Phase 1: Order book and matching engine
- A price-time-priority limit order book with MARKET, LIMIT, GTC, IOC, FOK, cancel and amend, plus
  every reject reason.
- A naive reference book and a 1M-operation differential test under sanitizers.
- Google Benchmark workloads.
- `finforge-sim` (Demo 2), with its expected output kept as a golden file.

### Phase 2: Event engine and exchange simulation
- A discrete-event simulator with a total event order `(ts, class, instrument, seq)` and a latency
  model.
- An append-only event journal whose SHA-256 is the run fingerprint (pinned in CI).
- Progress and cancel hooks.
- Synthetic multi-agent order flow on a portable PRNG.

### Phase 3: Historical data pipeline
- Ingest of Binance klines and bookTicker plus generic CSV/Parquet, with exact text→integer
  conversion.
- Structured validation reports with a reject / drop / flag policy per problem.
- Content-hash dataset ids.
- Chunked, zero-copy replay with a k-way merge across instruments and bounded memory.

### Phase 4: Execution models and portfolio ledger
- Candle and Quote execution models, fees, slippage, a volume cap, and pre-trade risk checks with
  reserved cash.
- A cost-basis ledger with shorts, flips and borrow costs, reconciled after every run.
- Hand-computed golden tests, look-ahead tests, and 1000 seeded randomised runs.

### Phase 5: Python strategy SDK and end-to-end backtest
- `Strategy` / `Context` SDK with read-only, look-ahead-safe `history()`.
- `finforge backtest`, with a run manifest and Parquet artefacts that are byte-identical across
  repeated runs.
- Reference strategies: MA crossover, mean reversion, momentum and pairs.

### Phase 6: Analytics and experiment comparison
- `report.json`: returns and risk metrics cross-checked against empyrical, FIFO trades, exposure,
  and a buy-and-hold benchmark.
- Run comparison, and parameter sweeps on a process pool with in-sample / out-of-sample columns.

### Phase 7: Backend, persistence and jobs
- FastAPI service with datasets, strategies (JSON Schema parameters), runs, results, compare, and
  SSE progress.
- Postgres metadata, plus a job queue with `SKIP LOCKED`, leases, heartbeats, crash recovery and
  cancel.
- Hardened uploads: size cap, content sniffing, server-generated paths. Only built-in strategies run.

### Phase 8: Research dashboard
- Next.js dashboard for the whole Demo 1 workflow, with a typed client generated from OpenAPI.
- Server-side downsampling: a one-year 1-minute equity curve draws in under 1 s.
- Playwright end-to-end test in CI.
- Verification pass: fixes to Book-model amend checks, MARKET amends, SDK amend tracking, the
  compare split boundary, SSE connection use, the upload cap, and SSE reconnects.

### Phase 9: Performance, hardening and release
- Measured against every PLAN target (`docs/benchmarks.md`, `scripts/bench.py`, a nightly
  benchmark workflow).
- SHA-256 via the x86 SHA extensions (a no-op 1M-bar backtest went from ~1.1 s to ~0.5 s).
- Fixed a leak of every finished run's engine.
- Exact quote references for execution cost.
- One-sided quotes no longer cancel MARKET/IOC/FOK orders.
- Exact SDK conversions, Parquet doubles in scientific notation, worker resilience to database
  blips, and stricter numeric bounds.
- The one-command Docker Compose stack, with a CI job that runs Demo 1 against it.
- Architecture document, tutorial and README quickstart.
