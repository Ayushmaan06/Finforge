#!/usr/bin/env python3
"""Download the PLAN.md D5 datasets from data.binance.vision and verify every file's .CHECKSUM.

    python scripts/fetch_binance.py                      # everything in D5 (~100 MB + ~700 MB)
    python scripts/fetch_binance.py --bars --months 2024-01 2024-02
    python scripts/fetch_binance.py --quotes --days 2024-03-01

Re-runnable: files that already pass their checksum are skipped. Downloads go to a temporary name
and are renamed only after the checksum matches. Standard library only. Raw data stays out of git
(datasets/raw/ is ignored); check Binance's terms before redistributing anything.
"""

import argparse
import hashlib
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

BASE = "https://data.binance.vision/data"
DEFAULT_OUT = Path(__file__).resolve().parent.parent / "datasets" / "raw" / "binance"
SPOT_SYMBOLS = ("BTCUSDT", "ETHUSDT")
DEFAULT_MONTHS = [f"{y}-{m:02d}" for y in (2023, 2024) for m in range(1, 13)]
# Futures bookTicker exists only for 2023-05-16 .. 2024-03-30 (verified 2026-11-11, PLAN D5).
DEFAULT_DAYS = ["2024-03-01", "2024-03-02"]
USER_AGENT = "finforge-fetch/1"


def _get(url: str, retries: int = 3) -> bytes:
    for attempt in range(retries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(req, timeout=60) as r:
                return r.read()
        except (urllib.error.URLError, TimeoutError):
            if attempt == retries - 1:
                raise
            time.sleep(2**attempt)
    raise AssertionError("unreachable")


def _download(url: str, dest: Path, retries: int = 3) -> None:
    tmp = dest.with_name(dest.name + ".part")
    for attempt in range(retries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(req, timeout=60) as r, open(tmp, "wb") as f:
                while block := r.read(1 << 20):
                    f.write(block)
            tmp.replace(dest)
            return
        except (urllib.error.URLError, TimeoutError):
            if attempt == retries - 1:
                raise
            time.sleep(2**attempt)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def fetch(remote: str, local_dir: Path) -> Path:
    """Fetch one file and its checksum. Raises ValueError if the checksum does not match."""
    local_dir.mkdir(parents=True, exist_ok=True)
    name = remote.rsplit("/", 1)[1]
    dest = local_dir / name
    want = _get(f"{BASE}/{remote}.CHECKSUM").decode().split()[0].lower()
    if dest.exists() and sha256(dest) == want:
        print(f"ok (cached) {name}")
        return dest
    _download(f"{BASE}/{remote}", dest)
    got = sha256(dest)
    if got != want:
        dest.unlink()
        raise ValueError(f"{name}: checksum mismatch (got {got}, want {want})")
    (local_dir / f"{name}.CHECKSUM").write_text(f"{want}  {name}\n")
    print(f"ok {name} ({dest.stat().st_size / 1e6:.1f} MB)")
    return dest


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--bars", action="store_true", help="spot 1m klines (default: both sets)")
    ap.add_argument("--quotes", action="store_true", help="futures bookTicker + 1m klines")
    ap.add_argument("--symbols", nargs="+", default=list(SPOT_SYMBOLS))
    ap.add_argument("--months", nargs="+", default=DEFAULT_MONTHS, help="YYYY-MM for spot klines")
    ap.add_argument("--days", nargs="+", default=DEFAULT_DAYS, help="YYYY-MM-DD for futures data")
    a = ap.parse_args(argv)
    both = not (a.bars or a.quotes)

    failures = 0
    jobs: list[tuple[str, Path]] = []
    if a.bars or both:
        for sym in a.symbols:
            for month in a.months:
                jobs.append(
                    (
                        f"spot/monthly/klines/{sym}/1m/{sym}-1m-{month}.zip",
                        a.out / "spot" / "klines" / sym / "1m",
                    )
                )
    if a.quotes or both:
        for day in a.days:
            jobs.append(
                (
                    f"futures/um/daily/bookTicker/BTCUSDT/BTCUSDT-bookTicker-{day}.zip",
                    a.out / "futures" / "bookTicker" / "BTCUSDT",
                )
            )
            jobs.append(
                (
                    f"futures/um/daily/klines/BTCUSDT/1m/BTCUSDT-1m-{day}.zip",
                    a.out / "futures" / "klines" / "BTCUSDT" / "1m",
                )
            )
    for remote, local in jobs:
        try:
            fetch(remote, local)
        except (ValueError, urllib.error.URLError, TimeoutError) as e:
            print(f"FAILED {remote}: {e}", file=sys.stderr)
            failures += 1
    print(f"done: {len(jobs) - failures} ok, {failures} failed -> {a.out}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
