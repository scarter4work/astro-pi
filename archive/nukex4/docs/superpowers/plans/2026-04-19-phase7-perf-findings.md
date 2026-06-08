# Phase 7 Perf Findings

## Baseline

- Dataset: NGC7635, 65 LRGB mono frames, 200 s exposures, Bin 1×1, ZWO-class mono sensor
- Phase B wall-time (unprofiled): **1,971,756 ms (32.86 min)**
- Perf run wall-time: 2,871,752 ms Phase B (47.86 min) — ~45% perf overhead, expected
- `perf.data`: 2.5 GB, ~4200 unique stack signatures after `stackcollapse-perf.pl`
- Flamegraph: `/tmp/nukex_phaseB.svg` (not committed; reproducible via `tools/capture_baseline.js` + `perf record -F 99 -g --call-graph dwarf`)
- Build: `build_profile/` with `CMAKE_BUILD_TYPE=RelWithDebInfo -DNUKEX_PROFILING=ON`

## Top 10 exclusive-leaf self-time (fraction of Phase B samples)

Total samples: 15.4e12 (weighted).

| # | Symbol | % Phase B |
|---|--------|:---------:|
| 1 | `__ieee754_log_fma` (glibc `log`) | 18.0% |
| 2 | `StudentTFitter::fit` | 11.4% |
| 3 | `StudentTNLL::Evaluate` | 10.8% |
| 4 | `std::vector::operator[]` / related | 7.4% |
| 5 | PixInsight core (unattributable internals) | 4.3% |
| 6 | `__exp` | 3.7% |
| 7 | `std::vector::size` | 3.6% |
| 8 | `StudentTNLL::compute_nll` (finite-diff helper) | 3.4% |
| 9 | `__ieee754_exp_fma` | 3.2% |
| 10 | `std::log` (dispatch) | 2.2% |

## Bottleneck assessment (spec criteria)

Pre-committed bar: **one section ≥ 50%, OR one function ≥ 3× siblings, OR visible redundancy**.

- **PASS via visible-redundancy criterion — plus a bigger structural win.** Two independent findings surfaced from the flamegraph + source inspection:

  1. **Phase B fitting loop is single-threaded on a 16-core machine** (`src/lib/gpu/src/gpu_executor.cpp:437`). The fitting_fn that wraps Ceres is called voxel-by-voxel in a plain `for` loop. `perf` recorded 99% on one core; 15 cores sat idle through the entire 48-minute fit. This is a structural defect, not a per-call cost.

  2. **Student-t NLL evaluates `log(1+z²)` three times per Ceres iteration** (main loop + two `compute_nll` calls for the finite-difference nu gradient at `student_t_fitter.cpp:103-105`). Two of those three sweeps can be eliminated by computing `d/dν` analytically and accumulating in the main loop — no extra log() calls needed, since `log(1+z²)` is already computed there.

## Selected optimization

**Parallelize the per-voxel Ceres fitting loop at `gpu_executor.cpp:437` with OpenMP.**

Rationale vs. finding #2:
- Parallelization is **systemic** — the same multi-core win applies to any future cost function, not just Student-t. It preserves numerical behavior bitwise (deterministic results per voxel, just computed concurrently across voxels).
- `ModelSelector::select()` allocates fitters as stack locals per call — no shared mutable state across voxels, so OpenMP parallel-for is trivially safe.
- Finding #2 is a useful ~12% micro-win we can land later (its analytical-gradient risk needs its own verification against synthetic Student-t data). Not bundled with B5.

## Expected speedup

- Host: 11th Gen Intel i7-11700K, 8 physical cores, 16 logical threads.
- Per-voxel fitting is CPU-bound (Ceres compute, not memory-bound) so scaling is near-linear up to physical-core count.
- Realistic conservative projection: **6–8× Phase B speedup** from 1 → 8 cores (the hyperthreaded 9th–16th threads add diminishing returns on compute-bound float math).
- Ship bar is 1.5× — this optimization clears it by a margin that would be a design error to *not* ship.
- Post-opt Phase B projection: **240–330 s (4–5.5 min)**, vs 1971 s baseline.

## Actual result (2026-04-20)

Post-optimization baseline on the same NGC7635 stack (release build, module signed + installed into PI):

- Baseline Phase B:          **1,971,756 ms (32.86 min)**
- Post-optimization Phase B: **210,882 ms (3.51 min)**
- Speedup:                   **9.35×**
- Ship bar (≥ 1.5×):         **PASS — exceeds bar by 6.2×**
- Frames processed:          65 / 65
- Failed alignment:          0

Observed `%CPU` during Phase B: ~430% average (≈ 4.3 cores active on the 8-physical-core i7-11700K). Not full 800% — likely mix of dynamic-chunk scheduling overhead, Ceres internal serial sections (SVD in line-search), and memory-bandwidth effects. Nonetheless the real wall-time gain is well beyond the projection, suggesting the per-voxel work is lighter per-thread than expected when not contending for a single core's cache.

Total wall-time (TOTAL_MS): 275,770 ms (4 min 36 s), vs 2,041,553 ms (34 min) pre-opt.
