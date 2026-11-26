# Benchmarks

How fast FinForge is, how that was measured, and how to measure it again. The targets are the
PLAN §10 targets. Why the engine looks the way it does, including the experiments, is in
[Optimisations](#optimisations).

## Reproduce

```bash
cmake --workflow --preset release          # builds build/release/engine/finforge_bench
uv pip install -e .                        # the Python package (rebuilds the extension)
python scripts/bench.py --reps 10          # everything; real data if datasets/ has it, else synthetic
python scripts/bench.py --quick            # smoke run on the synthetic demo data (what CI runs)
```

`bench.py` prints a markdown table and can write JSON (`--out`). It runs:

- **C++** (`finforge_bench`, Google Benchmark), median of the repetitions:
  - the four order-book workloads (100 000 pre-generated operations each, fixed seeds);
  - per-add latency percentiles;
  - Mode A over 1 M bars.
- **Python**, each measurement in a fresh process, so its peak RSS (`VmHWM`) is its own:
  - a backtest with an empty `on_bar` (the SDK floor);
  - the MA crossover;
  - ingest of the spot klines.

The `Benchmarks (smoke)` workflow runs `--quick` on demand and nightly. It only proves the
benchmarks still build and run: shared CI runners are too noisy to record.

## Method

- **Workloads are deterministic.** Order streams and the Mode A bar walk use fixed seeds, and
  generating them is outside the timed region (Mode A generates bars on the fly, which costs about
  2 % of that run).
- **Medians, not means.** At least 5 repetitions, 10 for recorded numbers. CV is reported by
  `finforge_bench --benchmark_repetitions=N`.
- **Noise on the reference machine.** Run-to-run variation is 7–26 % (CV) on this laptop. Its CPU
  is hybrid (performance and efficiency cores), and Hyper-V schedules WSL's virtual CPUs onto
  either kind, so `taskset` inside WSL cannot pin a core type. Two consequences:
  - Before/after comparisons are **interleaved** (A, B, A, B … in one session).
  - A change is only kept if it wins by **≥ 10 %**, with no other workload slower beyond the noise.
- **Latency** is measured per `submit` with `steady_clock`. The median cost of an empty timing pair
  (~20 ns) is subtracted. Cancels run untimed so the book evolves as in the throughput workload.
- **Determinism is the guard.** Every optimisation must leave the DET-1 pinned journal hash and the
  differential test unchanged; a changed hash means behaviour changed.

## Reference machine

Intel Core i5-13420H (4 P + 4 E cores, 12 threads), 7.6 GiB RAM for WSL2 Ubuntu 24.04 (kernel
6.18, microsoft-standard). GCC 13.3, `-O3`, release preset, no sanitizers. Python 3.12.3, numpy 2,
pyarrow. A laptop, not isolated.

## Results against PLAN §10 (2026-11-26, commit e872b8b, 10 repetitions)

| Target | Measured | |
|---|---|---|
| Book, mixed operations ≥ 1 M ops/s (single thread) | Mixed: 7.3 M ops/s (across runs: 4.5–9.8 M) | ✓ |
| Book, p99 add < 2 µs | passive adds p99 190 ns; adds that match p99 238 ns (p999 436 / 622 ns) | ✓ |
| Engine, C++ no-op strategy, 1 M bars < 1 s | 0.52 s (across runs 0.43–0.73 s) | ✓ |
| Engine, Python MA crossover, 1 M bars < 30 s | 16.6 s (34.9 s for the 2.1 M-bar dataset) | ✓ |
| Ingest and validate 10 M rows < 60 s (quote data, see note) | bookTicker: 442 k rows/s → 10 M in ~23 s | ✓ |
| Replay memory bounded at 10× the chunk size | Phase 3: 3 M quotes add 34 MiB peak RSS when streamed | ✓ |

Note on ingest: spot kline CSVs run at 128–180 k rows/s, i.e. 56–78 s per 10 M rows. Exact
text→integer parsing dominates. The target was re-baselined to quote data (PLAN §15 v1.3), and
klines are best effort.

### All measurements

| C++ (`finforge_bench`) | Median | Throughput |
|---|---|---|
| AddHeavy | 28.3 ms / 100 k ops | 3.42 M ops/s |
| CancelHeavy | 8.7 ms | 11.07 M ops/s |
| MatchHeavy | 10.6 ms | 8.85 M ops/s |
| Mixed | 13.2 ms | 7.26 M ops/s |
| Mode A, no-op strategy, 1 M bars | 519 ms | 1.88 M bars/s |
| Mode A, trading strategy (10 k orders), 1 M bars | 716 ms | 1.34 M bars/s |

| Python (`bench.py`, real data) | Wall | Rate | Peak RSS |
|---|---|---|---|
| Backtest, empty `on_bar`, 2.1 M bars (BTCUSDT + ETHUSDT, 2023–2024) | 13.4 s | 157 k bars/s | 762 MiB |
| Backtest, MA crossover (30/240), same bars | 34.9 s | 60 k bars/s | 809 MiB |
| Ingest spot klines (48 monthly zips, 2.1 M rows) | 11.7 s | 180 k rows/s | 236 MiB |
| Ingest futures bookTicker (2 days, 67.1 M rows) | 151.8 s | 442 k rows/s | 404 MiB |

The Python backtests are the strategy run alone (`benchmark: false`). A normal `run()` adds a
buy-and-hold benchmark run (about the cost of the empty strategy) and `report.json` (~1 s).

Peak RSS of a backtest grows with its length, by design: the strategy's bar history (numpy columns)
and the equity curve (one point per bar timestamp) are kept for the whole run. That is ~400 MiB
during the 2.1 M-bar run, plus the artefact tables at the end.

## Optimisations

Each one was found by profiling, measured before and after, and checked against the
determinism hash.

| Change | Found by | Before → after | Kept |
|---|---|---|---|
| SHA-256 compression with the x86 SHA extensions (runtime CPUID check, portable fallback, a test that both agree) | perf: `Sha256::block` was 55 % of a no-op Mode A run | 1 M bars 0.92–1.18 s → 0.55 s | ✓ byte-identical hash |
| `Context` holds a weak reference to the engine | RSS sampled per run phase: +55 MiB per run, never freed | leak gone; one run's peak 1.24 GiB → 0.78 GiB | ✓ (a bug fix) |
| MA crossover: `sum()/n` instead of `mean()` | py-spy: numpy's `mean()` wrapper ~45 % of the run | one year, one symbol: 15.2 s → 13.2 s (−13 %) | ✓ bit-identical |
| Flat price ladder, intrusive FIFO, order pool (PLAN D11) | — | not tried | deferred post-v1: the simple `std::map` book beats every target by 2.5–10× |
| Parallel per-file ingest | — | not tried | target re-baselined instead |

Profiling recipes (unprivileged WSL2: no hardware counters, `perf_event_paranoid = 2`):

```bash
cmake --workflow --preset profile   # release flags + frame pointers
perf record -e cpu-clock:u -F 1999 --call-graph fp --user-callchains -o out.data -- \
    build/profile/engine/finforge_bench --benchmark_filter=ModeA_NoOp --benchmark_min_time=1x
perf report -i out.data --no-children --stdio -g none
py-spy record --native --format raw -o out.folded -- python my_backtest.py   # Python + C++ frames
```

DWARF call graphs (`--call-graph dwarf`) and kernel samples need a lower `perf_event_paranoid`.

## History

- **Phase 1 baseline (2026-11-25):** AddHeavy 2.03 M, CancelHeavy 5.32 M, MatchHeavy 4.27 M,
  Mixed 3.75 M ops/s on the same machine. The same code measured about 2× faster in Phase 9, so the
  difference is the machine's state (power mode, scheduling), not the code. Those numbers are not
  comparable with the table above, which is why everything was re-measured in one session.
- **Phase 2 simulator:** `finforge-sim --synthetic --seed 42 --events 1000000` took 1.05–1.57 s
  (`/usr/bin/time`). Its journal hashing now uses the SHA extensions too.
