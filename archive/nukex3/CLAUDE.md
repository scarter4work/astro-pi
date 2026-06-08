# NukeX v3

Global context (build, signing, release workflow) is in `~/.claude/CLAUDE.md`.

## Project-Specific

- **Build**: `mkdir build && cd build && cmake .. && make -j$(nproc)` or `make release`
- **Tests**: `cd build && ctest --output-on-failure`
- **Design doc**: `docs/plans/2026-03-05-nukex-v3-design.md`
- **Dependencies** (all header-only, vendored in `third_party/`): xtensor, xsimd, xtl, Boost.Math, LBFGSpp, Eigen 5, Catch2 v3
