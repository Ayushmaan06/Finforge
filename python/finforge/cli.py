"""`finforge` command line.

    finforge backtest examples/ma_cross.yaml [--run-dir DIR]
    finforge report RUN_DIR                      # (re)write report.json
    finforge compare RUN_DIR [RUN_DIR ...] [--out DIR]
    finforge sweep examples/ma_cross_sweep.yaml [--out DIR]
    finforge data {ingest,validate,info,verify,replay} ...

Exit codes: 0 ok, 1 error, 2 rejected dataset (data), 3 failed backtest.
"""

import argparse
import json
import sys

from pydantic import ValidationError


def _backtest(argv: list[str]) -> int:
    from finforge.backtest import run
    from finforge.config import BacktestConfig

    ap = argparse.ArgumentParser(prog="finforge backtest")
    ap.add_argument("config", help="YAML config")
    ap.add_argument(
        "--run-dir", help="output directory (default: <output_dir>/<time>-<name>-<hash>)"
    )
    a = ap.parse_args(argv)
    try:
        config = BacktestConfig.from_yaml(a.config)
        result = run(config, run_dir=a.run_dir)
    except (ValidationError, ValueError, KeyError, FileNotFoundError, FileExistsError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    m = result.manifest
    s = m["summary"]
    print(f"status       {m['status']}")
    print(f"run dir      {result.run_dir}")
    print(f"strategy     {m['strategy']['name']} {json.dumps(m['strategy']['params'])}")
    print(f"dataset      {m['dataset']['name']} ({m['dataset']['id'][:12]})")
    print(
        f"events       {m['counts']['events_processed']}  orders {m['counts']['orders']}  "
        f"fills {m['counts']['fills']}"
    )
    print(
        f"equity       {s['initial_equity']:.2f} -> {s['final_equity']:.2f} ({s['return']:+.4%}), "
        f"fees {s['total_fees']:.2f}"
    )
    print(f"reconciles   {m['reconciles']}")
    print(f"journal      {m['journal_hash']}")
    print(f"wall time    {m['timings']['wall_seconds']} s")
    if m["error"]:
        print(m["error"]["traceback"], file=sys.stderr)
        return 3
    return 0


def _report(argv: list[str]) -> int:
    from finforge.analytics.report import write_report

    ap = argparse.ArgumentParser(prog="finforge report")
    ap.add_argument("run_dir")
    a = ap.parse_args(argv)
    from pathlib import Path

    bench = Path(a.run_dir) / "benchmark"
    rep = write_report(a.run_dir, bench if (bench / "equity.parquet").exists() else None)
    p = rep["performance"]
    print(
        f"return {p['total_return']:+.4%}  sharpe {p['sharpe']}  max dd {p['max_drawdown']:.4%}  "
        f"trades {rep['trades']['count']}"
    )
    return 0


def _compare(argv: list[str]) -> int:
    import pyarrow.parquet as pq

    from finforge.analytics.compare import compare

    ap = argparse.ArgumentParser(prog="finforge compare")
    ap.add_argument("run_dirs", nargs="+")
    ap.add_argument("--out", help="write compare.json and series.parquet here")
    a = ap.parse_args(argv)
    try:
        c = compare(a.run_dirs)
    except FileNotFoundError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(c.format_table())
    if a.out:
        from pathlib import Path

        out = Path(a.out)
        out.mkdir(parents=True, exist_ok=True)
        (out / "compare.json").write_text(json.dumps(c.rows, indent=2) + "\n")
        pq.write_table(c.series, out / "series.parquet")
    return 0


def _sweep(argv: list[str]) -> int:
    from finforge.analytics.sweep import SweepConfig, format_results, run_sweep

    ap = argparse.ArgumentParser(prog="finforge sweep")
    ap.add_argument("config")
    ap.add_argument("--out", help="sweep directory (default: <output_dir>/<time>-<strategy>)")
    a = ap.parse_args(argv)
    try:
        table = run_sweep(SweepConfig.from_yaml(a.config), a.out)
    except (ValidationError, ValueError, FileNotFoundError, FileExistsError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(format_results(table))
    return 0


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0 if argv else 1
    cmd, rest = argv[0], argv[1:]
    if cmd == "backtest":
        return _backtest(rest)
    if cmd == "report":
        return _report(rest)
    if cmd == "compare":
        return _compare(rest)
    if cmd == "sweep":
        return _sweep(rest)
    if cmd == "data":
        from finforge.data.__main__ import main as data_main

        return data_main(rest)
    print(f"unknown command {cmd!r}\n{__doc__}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
