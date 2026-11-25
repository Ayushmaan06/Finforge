"""Command line: python -m finforge.data {ingest,validate,info,verify,replay} ...

Exit codes: 0 ok, 2 dataset rejected by validation, 1 other errors.
"""

import argparse
import json
import sys
import time

from finforge.data.dataset import Dataset, replay
from finforge.data.ingest import DatasetSpec, ingest
from finforge.data.validate import DatasetRejected, Report


def _print_report(report: Report) -> None:
    d = report.to_dict(with_samples=True)
    print(
        f"validation: {d['status']}  rows read {d['rows_read']}, written {d['rows_written']}, "
        f"dropped {d['rows_dropped']}"
    )
    for f in d["findings"]:
        extra = "".join(
            f", {k}={v}"
            for k, v in f.items()
            if k not in ("code", "kind", "symbol", "action", "count", "samples")
        )
        first = f["samples"][0] if f["samples"] else None
        where = f"  e.g. {first['file']} row {first['row']}" if first else ""
        print(
            f"  {f['action']:6} {f['code']:18} {f['kind']}/{f['symbol']}: "
            f"{f['count']}{extra}{where}"
        )


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m finforge.data")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("ingest", help="ingest a dataset spec into a normalised dataset")
    p.add_argument("spec")
    p.add_argument("--out", required=True)
    p.add_argument("--root", help="base directory for source paths (default: the spec's directory)")
    p.add_argument("--force", action="store_true", help="replace --out if it exists")

    p = sub.add_parser("validate", help="validate a dataset spec without writing anything")
    p.add_argument("spec")
    p.add_argument("--root")
    p.add_argument("--json", action="store_true", help="print the full report as JSON")

    p = sub.add_parser("info", help="summarise a dataset")
    p.add_argument("dataset")

    p = sub.add_parser("verify", help="recompute content hashes and the dataset id")
    p.add_argument("dataset")

    p = sub.add_parser("replay", help="replay a dataset with a no-op strategy")
    p.add_argument("dataset")
    p.add_argument("--chunk-rows", type=int, default=65_536)

    a = ap.parse_args(argv)
    try:
        if a.cmd in ("ingest", "validate"):
            spec = DatasetSpec.load(a.spec, a.root)
            t0 = time.perf_counter()
            try:
                res = ingest(
                    spec, a.out if a.cmd == "ingest" else None, force=getattr(a, "force", False)
                )
            except DatasetRejected as e:
                _print_report(e.report)
                return 2
            if getattr(a, "json", False):
                print(json.dumps(res.report.to_dict(), indent=2))
            else:
                _print_report(res.report)
            if res.path is not None and res.manifest is not None:
                print(f"dataset {res.manifest['id']}")
                print(f"written to {res.path} in {time.perf_counter() - t0:.1f}s")
        elif a.cmd == "info":
            ds = Dataset(a.dataset)
            m = ds.manifest
            print(f"{m['name']}  id {m['id']}")
            print(f"time range (ns): {m['time_range_ns']}")
            for f in m["files"]:
                print(f"  {f['kind']:6} {f['symbol']:12} rows {f['rows']:>10}  {f['path']}")
            print(f"validation: {m['validation']['status']}")
            for n in m.get("notes", []):
                print(f"note: {n}")
        elif a.cmd == "verify":
            Dataset(a.dataset).verify()
            print("ok")
        elif a.cmd == "replay":
            t0 = time.perf_counter()
            r = replay(a.dataset, chunk_rows=a.chunk_rows)
            dt = time.perf_counter() - t0
            n = r.counts["events_processed"]
            print(f"status {r.status}  counts {r.counts}")
            print(f"journal {r.journal_hash}")
            print(f"{n} events in {dt:.2f}s ({n / dt / 1e6:.2f} M events/s)" if dt > 0 else "")
    except (ValueError, FileNotFoundError, FileExistsError, KeyError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
