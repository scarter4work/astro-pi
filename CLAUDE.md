# NukeX v3

## Build
- CMake: `mkdir build && cd build && cmake .. && make -j$(nproc)`
- Make: `make release`
- Tests: `cd build && ctest --output-on-failure`

## Release Workflow (MUST follow in order)
1. Bump `MODULE_VERSION_BUILD` in `src/NukeXModule.cpp`
2. Update `repository/updates.xri` title + description with new version
3. `make clean && make release` — clean rebuild
4. `cd build && ctest --output-on-failure` — verify all tests pass
5. `make package` — signs module, creates tarball, updates SHA1, signs XRI
6. `sudo make install` or `sudo cp NukeX-pxm.so NukeX-pxm.xsgn /opt/PixInsight/bin/`
7. Commit version bump + package files together in one commit
8. `git push`
- **NEVER push without completing steps 1-7**
- **NEVER skip the version bump or packaging step**

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
