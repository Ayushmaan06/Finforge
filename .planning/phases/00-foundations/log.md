# Phase 00 — Log

## 2026-11-12 — Move to WSL2, data download
**Done:** Project moved from `D:\finforge` to `~/finforge` in WSL2 Ubuntu 24.04. Toolchain installed.
D5 datasets downloaded with `scripts/fetch_binance.sh`; all 52 files pass their `.CHECKSUM` (657 MB).
**Decisions:** Quotes come from USD-M futures bookTicker (spot has none) — PLAN v1.1.
**Next:** Start Phase 0 tasks.

## 2026-11-13 — Walking skeleton, CI, contracts
**Done:**
- Repo hygiene: `.gitignore` (build, venvs, all of `datasets/` except `fixtures/` and README, `temp/`),
  `.editorconfig`, `.clang-format` (Google base, 4-space, 100 cols), MIT LICENSE, README quickstart.
- CMake: top-level + `engine/` static lib `finforge::engine` with `engine_version()`
  (`<semver>+<git sha>.<build type>`, PLAN D6), GoogleTest v1.17.0 and pybind11 v2.13.6 via
  `FetchContent` with SHA-256-pinned tarballs. Options `FINFORGE_BUILD_TESTS`, `FINFORGE_BUILD_PYTHON`,
  `FINFORGE_SANITIZE`. Presets `dev` (Debug + ASan/UBSan) and `release`, with workflow presets.
- Python: `pyproject.toml` (scikit-build-core, dependency group `dev` = pytest + ruff 0.17.5), package
  `python/finforge`, extension `finforge._engine`, 2 pytest tests.
- CI `.github/workflows/ci.yml`: C++ release (gcc), dev+sanitizers (gcc and clang), Python, lint.
- `docs/contracts.md`: §5 expanded into numbered rules with a Test: line each, refinements listed (§12),
  open points (§13), test plan by component (§14). PLAN.md §15 v1.2 entry.

**Tool versions (WSL2 Ubuntu 24.04.1, kernel 6.18 WSL2):** gcc 13.3.0, clang 18.1.3, clang-format 18.1.3,
cmake 3.28.3, ninja 1.11.1, Python 3.12.3, uv 0.13.7, git (Ubuntu 24.04 package).

**Decisions:**
- pybind11 via `FetchContent` (D10) rather than the pip `pybind11` build requirement — keeps one
  dependency mechanism; cost is a network fetch on first build.
- Sanitizers and the Python module are mutually exclusive in CMake — an ASan-instrumented `.so` can't be
  imported by an uninstrumented interpreter (it would need `LD_PRELOAD=libasan`).
- Python tests run with `.venv/bin/pytest` rather than `uv run` — `uv run` would sync and rebuild the
  package on every call.
- Test presets set `noTestsAction: error` so a misconfigured build can't pass with zero tests.

**Problems:** First build ran 0 tests and reported success. Root cause: `enable_testing()` was inside
`engine/CMakeLists.txt`, so CTest in the build root had no test file. Fix: call it in the top-level
`CMakeLists.txt`, and fail on zero tests.

**Learned:** `ldd` on the test binary shows `libasan`/`libubsan` — the quick way to confirm sanitizers
are actually linked. FetchContent `URL_HASH` makes dependency downloads tamper-evident.

**Next:** Push to https://github.com/Ayushmaan06/Finforge and get CI green; then fresh-clone check and summary.md.

## 2026-11-13 — GitHub, CI green
**Done:** `gh` 2.45.0 installed; logged in as Ayushmaan06 via device code (scopes include `workflow`,
needed to push `.github/workflows/`). Pushed `main` to the private repo Ayushmaan06/Finforge.
CI run 43480045951: all 5 jobs green on first run (release gcc, dev+sanitizers gcc and clang, Python,
lint). Fresh local clone passes both exit-criteria commands. Bumped `actions/checkout` v4 → v7 and
`astral-sh/setup-uv` v6 → v10.3.3 (Node 20 deprecation warnings). summary.md written.
**Problems:** `gh auth login` failed in the user's terminal with `gzip: stdin: not in gzip format` — the
browser hand-off from WSL has no opener (`BROWSER` unset, no `wslview`/`xdg-open`). Fix: device-code flow
with `BROWSER=true`, code entered manually at github.com/login/device.
**Learned:** Pushing workflow files needs the `workflow` OAuth scope. setup-uv no longer has a `v10`
floating tag, so it must be pinned to an exact version.
**Next:** User reviews contracts.md §12 refinements; then start Phase 1 (`.planning/phases/01-order-book/plan.md`).
