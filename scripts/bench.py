"""Phase 9 performance measurements against PLAN §10 (docs/benchmarks.md).

    python scripts/bench.py                 # everything, real data if present, else the demo data
    python scripts/bench.py --quick         # CI smoke: few repetitions, demo dataset only
    python scripts/bench.py --out bench.json

Each Python measurement runs in a fresh subprocess, so its peak RSS (VmHWM) is its own. C++
numbers come from finforge_bench (Google Benchmark, median of the repetitions). Numbers from
shared CI runners are a smoke test only and are never recorded.
"""

from __future__ import annotations

import argparse
import json
import platform
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BENCH = ROOT / "build" / "release" / "engine" / "finforge_bench"
SPOT = ROOT / "datasets" / "normalized" / "binance-spot-1m"
SPOT_SPEC = ROOT / "datasets" / "specs" / "binance-spot-1m.json"

EMPTY_STRATEGY = """
from finforge.sdk import Strategy

class Empty(Strategy):
    calls = 0
    def on_bar(self, ctx, bar):
        Empty.calls += 1
"""


def _peak_rss_mib() -> float:
    for line in Path("/proc/self/status").read_text().splitlines():
        if line.startswith("VmHWM:"):
            return int(line.split()[1]) / 1024
    return float("nan")


# --- measurements, each run in its own process -----------------------------------------------


def _backtest(dataset: str, universe: list[str], strategy: str, params: dict) -> dict:
    from finforge.backtest import run

    with tempfile.TemporaryDirectory() as tmp:
        if strategy == "empty":
            path = Path(tmp) / "empty.py"
            path.write_text(EMPTY_STRATEGY)
            strategy = f"{path}:Empty"
        cfg = {
            "dataset": dataset,
            "universe": universe,
            "initial_capital": "1000000",
            # The strategy alone: run() would add a buy-and-hold benchmark run (about the cost of
            # the empty strategy) and report.json; PLAN §10 targets the strategy run.
            "benchmark": False,
            "strategy": {"name": strategy, "params": params},
            "output_dir": str(Path(tmp) / "runs"),
        }
        t0 = time.perf_counter()
        r = run(cfg)
        wall = time.perf_counter() - t0
    assert r.status == "completed", r.manifest.get("error")
    manifest = json.loads((Path(dataset) / "manifest.json").read_text())
    ids = {i["instrument_id"] for i in manifest["instruments"] if i["symbol"] in universe}
    bars = sum(
        f["rows"] for f in manifest["files"] if f["kind"] == "bars" and f["instrument_id"] in ids
    )
    return {
        "seconds": wall,
        "engine_seconds": r.manifest["timings"]["wall_seconds"],
        "items": bars,
        "unit": "bars",
    }


def _ingest(spec: str) -> dict:
    from finforge.data import ingest

    with tempfile.TemporaryDirectory() as tmp:
        t0 = time.perf_counter()
        res = ingest(spec, Path(tmp) / "ds", force=True)
        wall = time.perf_counter() - t0
        manifest = json.loads((Path(res.path) / "manifest.json").read_text())
    rows = sum(int(f["rows"]) for f in manifest["files"])
    return {"seconds": wall, "items": rows, "unit": "rows"}


def one(name: str, args: dict) -> None:
    fn = {"backtest": _backtest, "ingest": _ingest}[name]
    result = fn(**args)
    result["peak_rss_mib"] = round(_peak_rss_mib(), 1)
    print(json.dumps(result))


def _subprocess(name: str, **args) -> dict:
    out = subprocess.run(
        [sys.executable, __file__, "--one", name, json.dumps(args)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return json.loads(out.strip().splitlines()[-1])


# --- driver --------------------------------------------------------------------------------------


def cpp(reps: int, quick: bool) -> list[dict]:
    if not BENCH.exists():
        sys.exit(f"{BENCH} missing: run `cmake --workflow --preset release` first")
    cmd = [str(BENCH), "--benchmark_format=json"]
    cmd += (
        ["--benchmark_min_time=1x"]
        if quick
        else [f"--benchmark_repetitions={reps}", "--benchmark_report_aggregates_only=true"]
    )
    data = json.loads(subprocess.run(cmd, check=True, capture_output=True, text=True).stdout)
    rows = []
    for b in data["benchmarks"]:
        if not quick and b.get("aggregate_name") != "median":
            continue
        counters = {k: b[k] for k in ("items_per_second", "p50_ns", "p99_ns", "p999_ns") if k in b}
        rows.append({"name": b["run_name"], "ms": round(b["real_time"], 2), **counters})
    return rows


def demo_dataset(tmp: Path) -> Path:
    from finforge.data.demo import make_demo_dataset

    return make_demo_dataset(tmp / "demo", days=365, symbols=("DEMO", "DEMO2"))


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    p.add_argument("--quick", action="store_true", help="CI smoke run (demo data, 1 repetition)")
    p.add_argument("--reps", type=int, default=5, help="C++ benchmark repetitions (median)")
    p.add_argument("--out", type=Path, help="also write the results as JSON")
    p.add_argument("--one", nargs=2, help=argparse.SUPPRESS)
    a = p.parse_args()
    if a.one:
        return one(a.one[0], json.loads(a.one[1]))

    with tempfile.TemporaryDirectory() as tmp:
        real = SPOT.exists() and not a.quick
        if real:
            dataset, universe = str(SPOT), ["BTCUSDT", "ETHUSDT"]
        else:
            dataset, universe = str(demo_dataset(Path(tmp))), ["DEMO", "DEMO2"]
        print(f"dataset: {dataset} ({'real' if real else 'synthetic demo'})", file=sys.stderr)

        results: dict = {
            "machine": {
                "cpu": _cpu(),
                "python": platform.python_version(),
                "commit": _git("rev-parse", "--short", "HEAD"),
            },
            "cpp": cpp(a.reps, a.quick),
            "python": {},
        }
        py = results["python"]
        py["empty_on_bar"] = _subprocess(
            "backtest", dataset=dataset, universe=universe, strategy="empty", params={}
        )
        py["ma_cross"] = _subprocess(
            "backtest",
            dataset=dataset,
            universe=universe,
            strategy="ma_cross",
            params={"fast": 30, "slow": 240, "qty": "0.01"},
        )
        if real and SPOT_SPEC.exists():
            py["ingest_spot_1m"] = _subprocess("ingest", spec=str(SPOT_SPEC))

    _print(results)
    if a.out:
        a.out.write_text(json.dumps(results, indent=2) + "\n")


def _cpu() -> str:
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor()


def _git(*args: str) -> str:
    r = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True)
    return r.stdout.strip()


def _print(r: dict) -> None:
    m = r["machine"]
    print(f"\n{m['cpu']} · Python {m['python']} · commit {m['commit']}\n")
    print("| C++ benchmark | median ms | items/s | p50 / p99 / p999 ns |")
    print("|---|---|---|---|")
    for b in r["cpp"]:
        ips = f"{b['items_per_second'] / 1e6:.2f} M" if "items_per_second" in b else ""
        lat = f"{b['p50_ns']:.0f} / {b['p99_ns']:.0f} / {b['p999_ns']:.0f}" if "p99_ns" in b else ""
        print(f"| {b['name']} | {b['ms']} | {ips} | {lat} |")
    print("\n| Python measurement | seconds | items | per second | peak RSS MiB |")
    print("|---|---|---|---|---|")
    for name, v in r["python"].items():
        rate = v["items"] / v["seconds"] if v["seconds"] else 0
        print(
            f"| {name} | {v['seconds']:.2f} | {v['items']:,} {v['unit']} | "
            f"{rate:,.0f} | {v['peak_rss_mib']} |"
        )


if __name__ == "__main__":
    main()
