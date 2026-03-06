# NukeX v3

## Build
- CMake: `mkdir build && cd build && cmake .. && make -j$(nproc)`
- Make: `make release`
- Tests: `cd build && ctest --output-on-failure`

## Architecture
- PCL/C++17 PixInsight Process Module
- Two processes: NukeXStack (stacking) + NukeXStretch (stretching)
- See docs/plans/2026-03-05-nukex-v3-design.md for full design

## Dependencies (all header-only, vendored in third_party/)
- xtensor 0.26.x — 3D tensor (BSD-3)
- xsimd — SIMD for xtensor (BSD-3)
- xtl — xtensor support library (BSD-3)
- Boost.Math — distributions, special functions (BSL)
- LBFGSpp — L-BFGS optimizer (MIT)
- Eigen 5 — linear algebra for LBFGSpp (MPL-2.0)
- Catch2 v3 — testing (BSL)

## Conventions
- RAII everywhere — no raw new/delete
- float for storage, double for computation
- All statistical functions must be thread-safe / reentrant
- Follow v2 patterns from ~/projects/NukeX/src/ for PCL boilerplate
