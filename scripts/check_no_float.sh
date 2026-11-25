#!/usr/bin/env bash
# NUM-1 (docs/contracts.md): no floating point in the engine core. Fails if engine/src or
# engine/include mention float/double types or decimal-parsing functions that return them.
# Files listed in scripts/num1_allowlist.txt (one path per line, '#' comments) are skipped.
set -euo pipefail
cd "$(dirname "$0")/.."

allow=()
if [[ -f scripts/num1_allowlist.txt ]]; then
  while IFS= read -r line; do
    line="${line%%#*}"; line="${line// /}"
    [[ -n "$line" ]] && allow+=("--exclude=$(basename "$line")")
  done < scripts/num1_allowlist.txt
fi

pattern='\b(float|double|long double|std::sto[fd]|stold|strto[fd]|atof|_Float[0-9]+)\b'
if grep -rnE "${allow[@]}" "$pattern" engine/src engine/include; then
  echo "NUM-1: floating point found in the engine core (see matches above)." >&2
  exit 1
fi
echo "NUM-1: no floating point in engine/src or engine/include."
