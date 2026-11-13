# Phase 00 — Foundations and Contracts: Summary

## What was built

| Component | Where | Public interface |
|---|---|---|
| Engine library | `engine/` | `finforge::engine` (static, `-fPIC`); `std::string finforge::engine_version()` in `finforge/version.hpp` |
| C++ tests | `engine/tests/` | GoogleTest executable `finforge_engine_tests`, registered with CTest |
| Python extension | `python/bindings/module.cpp` | `finforge._engine.engine_version()` |
| Python package | `python/finforge/` | `finforge.engine_version`, `finforge.__version__` |
| Build | `CMakeLists.txt`, `CMakePresets.json`, `pyproject.toml` | presets `dev`, `release`; `uv pip install -e . --group dev` |
| CI | `.github/workflows/ci.yml` | 5 jobs on every push to `main` and every PR |
| Spec | `docs/contracts.md` | Numbered rules (NUM, TIME, INS, ORD, MAT, EVT, CND, QTE, BKM, FEE, SLP, ACC, DET, VER, MET) |

## How it works

```text
CMakeLists.txt ──► engine/ (finforge_engine.a) ──► engine/tests (gtest)  ──► ctest
        │                         │
        │ FINFORGE_BUILD_PYTHON   └──────────────► python/bindings/_engine.so
        ▼                                                 │ installed into
pyproject.toml (scikit-build-core) ─────────────────► python/finforge/ ──► pytest
```

- `cmake --workflow --preset dev` configures a Debug build with ASan and UBSan, builds it, and runs
  CTest. `release` does the same with optimisation and no sanitizers.
- `uv pip install -e .` makes scikit-build-core run CMake with `FINFORGE_BUILD_PYTHON=ON` and
  `FINFORGE_BUILD_TESTS=OFF` in Release mode, then installs `_engine.so` into the `finforge` package.
- The version string is generated at configure time from `project(VERSION)`, `git rev-parse HEAD` and
  `CMAKE_BUILD_TYPE` (`engine/src/build_info.hpp.in`), e.g. `0.1.0+0f8cc2230aa8.Release`. This is the
  engine half of PLAN D6.

## Key decisions

- **Dependencies via `FetchContent` with SHA-256-pinned tarballs** (D10) for GoogleTest and pybind11.
  Pinning by hash makes a changed upstream archive a build failure instead of a silent change.
  Alternative: pip's `pybind11` as a build requirement; rejected to keep one mechanism.
- **Sanitizers and the Python module are mutually exclusive** (CMake `FATAL_ERROR`). An ASan-built
  extension needs the ASan runtime loaded first, which a normal Python doesn't do. Python is tested in
  Release; memory safety is tested in the C++ sanitizer jobs.
- **Test presets fail when zero tests are found** — prevents a silently empty test run (see Gotchas).
- **Sanitizer job on both GCC and Clang** — they catch different things and the plan allows either.
- **Contracts refined, not changed.** 13 gaps in PLAN §5 were filled (contracts.md §12); 3 left open with
  proposed defaults and a deciding phase (contracts.md §13). PLAN.md v1.2.

## Concepts explained

- **Static library + `-fPIC`.** A shared object (the Python extension) can be loaded at any address, so
  every piece of code linked into it must be position-independent — including the static engine library.
- **AddressSanitizer** instruments every memory access and surrounds allocations with poisoned "red
  zones", so out-of-bounds reads, use-after-free and leaks abort with a stack trace.
  **UndefinedBehaviorSanitizer** checks signed overflow, bad shifts, misaligned access and so on.
  `-fno-sanitize-recover=all` turns every UBSan report into a test failure.
- **Why fixed-point.** `0.1 + 0.2 != 0.3` in binary floating point. Order matching needs exact equality
  of prices (they are map keys), and the ledger must reconcile to the last unit. Integers in ticks, lots
  and 1e-8 money units are exact, and give identical results on every machine (contracts.md §1).
- **Why determinism.** If the same inputs can give different outputs, no bug can be reproduced and no
  result can be trusted. Hashing the event journal turns "deterministic" into a checkable fact (DET-1).

## Testing

- C++: `Version.HasSemverGitShaAndBuildType` under Release and under ASan/UBSan (GCC and Clang).
- Python: the version comes from C++ and reports `Release`; `_engine` is a compiled `.so`.
- Lint: clang-format 18 (`--dry-run -Werror`), ruff check and ruff format.
- The test plan for the domain code is `docs/contracts.md` §14 plus a **Test:** line under every rule.

## Results

- Fresh clone (local): `cmake --preset dev && cmake --build --preset dev && ctest --preset dev` and
  `uv venv && uv pip install -e . --group dev && pytest` both pass.
- CI run 43480045951 on `0f8cc22`: all 5 jobs green; slowest is the Python job at about 1 minute.
- Toolchain versions are recorded in `log.md`.

## Gotchas & lessons

- `enable_testing()` in a subdirectory: CTest in the build root finds nothing and reports success.
  Put it in the top-level `CMakeLists.txt`.
- `ldd <test binary> | grep san` is the quick check that sanitizers are really linked.
- `uv run` syncs the project, which rebuilds the extension; use `.venv/bin/pytest` for quick test runs.
- From WSL, `gh auth login` can't open a browser; use the device-code flow and enter the code manually.
- Pushing `.github/workflows/*` needs the `workflow` token scope.

## Deviations from PLAN.md

- Exit criterion wording `cmake --preset dev && ctest` needs a build step in between; the README uses
  `cmake --workflow --preset dev` (configure, build, test in one command).
- Repository structure follows PLAN §8 except `engine/apps/` and `engine/bench/`, not needed until
  Phase 1. No PLAN change needed.
- Contract refinements: recorded in PLAN.md §15 v1.2.

## Open items carried forward

- **User review of contracts.md §12 refinements** before Phase 1 tests are written against them.
- OP-1 (market buy short of cash at fill) and OP-3 (orders open at the end of a run) → Phase 4;
  OP-2 (journal byte format) → Phase 2.
- Phase 1 adds Google Benchmark (D10) and the CI grep that enforces NUM-1 (no floating point in `engine/src`).
