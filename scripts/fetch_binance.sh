#!/usr/bin/env bash
# Download the D5 datasets (PLAN.md §3) from data.binance.vision and verify checksums.
# Re-runnable: files that already pass their checksum are skipped.
# ponytail: shell + curl for now; Phase 3 task 6 replaces this with scripts/fetch_binance.py.
set -euo pipefail

BASE=https://data.binance.vision/data
OUT="${1:-$(dirname "$0")/../datasets/raw/binance}"

fetch() {  # fetch <remote path under /data> <local dir>
  local url="$BASE/$1" dir="$OUT/$2" f
  f=$(basename "$1")
  mkdir -p "$dir"
  if [ -f "$dir/$f" ] && [ -f "$dir/$f.CHECKSUM" ] && (cd "$dir" && sha256sum -c --quiet "$f.CHECKSUM" 2>/dev/null); then
    echo "ok (cached) $f"; return
  fi
  curl -fsSL --retry 3 -o "$dir/$f.CHECKSUM" "$url.CHECKSUM"
  curl -fsSL --retry 3 -o "$dir/$f" "$url"
  (cd "$dir" && sha256sum -c --quiet "$f.CHECKSUM") && echo "ok $f"
}

# Bars: spot 1m klines, BTCUSDT + ETHUSDT, 2023-01..2024-12 (~2 MB/month)
for sym in BTCUSDT ETHUSDT; do
  for y in 2023 2024; do
    for m in 01 02 03 04 05 06 07 08 09 10 11 12; do
      fetch "spot/monthly/klines/$sym/1m/$sym-1m-$y-$m.zip" "spot/klines/$sym/1m"
    done
  done
done

# Quotes: USD-M futures bookTicker (~340 MB/day) + same-day futures 1m klines
for d in 2024-03-01 2024-03-02; do
  fetch "futures/um/daily/bookTicker/BTCUSDT/BTCUSDT-bookTicker-$d.zip" "futures/bookTicker/BTCUSDT"
  fetch "futures/um/daily/klines/BTCUSDT/1m/BTCUSDT-1m-$d.zip" "futures/klines/BTCUSDT/1m"
done

echo "done -> $OUT"
