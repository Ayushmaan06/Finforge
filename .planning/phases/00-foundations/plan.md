# Phase 00 — Foundations and Contracts

## Goal
A working toolchain end to end (C++ → Python → tests → CI), and written contracts before any domain
code. (PLAN.md §6, Phase 0.)

## Learning goals
- CMake targets, presets and `FetchContent`; why `enable_testing()` must be at the top level.
- How a C++ static library becomes a Python extension: pybind11 + scikit-build-core, `-fPIC`.
- GitHub Actions: jobs, matrices, caching-free reproducible builds.
- Sanitizers: what ASan and UBSan catch, why they can't be loaded into a normal Python.
- Why fixed-point arithmetic and determinism matter in finance (docs/contracts.md §1, §10).

## Tasks
- [x] 1. `git init`, `.gitignore`, `.editorconfig`, LICENSE (MIT), README stub
- [x] 2. WSL2 Ubuntu toolchain (D9); versions recorded in log.md
- [x] 3. Walking skeleton: `engine/` static lib + GoogleTest; pybind11 `finforge._engine`; pytest calls C++
- [x] 4. CI: Release build + tests; Debug ASan/UBSan (gcc + clang) + tests; pytest; clang-format + ruff
- [x] 5. `docs/contracts.md` from PLAN §5, every rule testable
- [x] 6. Test plan: invariants per component (contracts.md §14, plus a Test: line per rule)
- [x] 7. Push to GitHub, CI green on all jobs
- [x] 8. Fresh-clone check in WSL2, then `summary.md`

## Exit criteria
- [x] On a fresh clone in WSL2, `cmake --preset dev && ctest` and `uv pip install -e . && pytest` both pass.
- [x] CI is green on all jobs, including the sanitizer job.
- [x] `docs/contracts.md` covers every item in §5.
- [x] `.planning/phases/00-foundations/summary.md` is written.

## Open questions / risks
- GitHub repo access from WSL (no `gh`, no SSH key yet).
- Contract refinements (contracts.md §12) need the user's review before Phase 1 tests are written against them.
