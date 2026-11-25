# Benchmarks

Order book throughput, measured with Google Benchmark (`engine/bench/book_bench.cpp`).

## Method

- Each workload is a **pre-generated** stream of 100 000 operations with a fixed seed
  (`std::mt19937_64`, mapped to ranges by hand so streams are identical on every platform).
  Generation happens outside the timed region.
- One iteration replays the whole stream into a fresh `MatchingEngine` (construction included,
  negligible next to 100k operations). Results are passed to `benchmark::DoNotOptimize`.
- Reported: mean and coefficient of variation over 5 repetitions; `items_per_second` = operations/s.
- Release build (`-O3`, no sanitizers, no `FINFORGE_EXPENSIVE_CHECKS`). CI runs a smoke pass only;
  CI numbers are never recorded here (shared runners are noisy).

| Workload | Stream | What it stresses |
|---|---|---|
| AddHeavy | 90% passive GTC limits over 50 levels per side, 10% cancels | level insertion, book growth to ~90k orders |
| CancelHeavy | 2 000 passive orders to seed, then 40% adds / 60% cancels of random open orders | id lookup, unlink from the middle of a FIFO, level removal |
| MatchHeavy | 55% passive within 5 ticks, 45% IOC limits crossing up to 3 ticks | matching loop, level sweeps |
| Mixed | 50% passive (20 levels), 30% cancels, 20% IOC crossing up to 5 ticks | all of the above |

## Baseline — Phase 1 (2026-11-25, commit of the Phase 1 PR)

Machine: Intel Core i5-13420H (12 threads), 8 GB RAM, WSL2 Ubuntu 24.04, GCC 13.3.0, `-O3`.
Laptop, not pinned or isolated, so expect a few percent of noise (see CV).

| Workload | Mean time / 100k ops | Throughput | CV | Trades per op |
|---|---|---|---|---|
| AddHeavy | 50.2 ms | 2.03 M ops/s | 4.1% | 0 |
| CancelHeavy | 19.2 ms | 5.32 M ops/s | 4.1% | 0 |
| MatchHeavy | 23.9 ms | 4.27 M ops/s | 2.5% | 0.38 |
| Mixed | 27.3 ms | 3.75 M ops/s | 6.8% | 0.14 |

Notes:
- AddHeavy is the slowest; the book grows to ~90k resting orders over only 100 price levels.
  Hypothesis (not yet profiled): the cost is the `orders_` vector growing and reallocating, and
  cache misses on the ~90k order records.
- Nothing has been optimised yet. Phase 9 revisits this with a profiler; this table is the
  reference any change must be compared against.
