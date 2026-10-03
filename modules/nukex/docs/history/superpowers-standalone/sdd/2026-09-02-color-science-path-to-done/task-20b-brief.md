### Task 20b: Fitting — data-derived estimate for n < 3 (no zeroed voxels)

**Why.** `KDEFitter::fit` (`src/lib/fitting/src/kde_fitter.cpp:128-131`) returns a default-constructed `FitResult` for `n < 3`; `ModelSelector::select` copies that zeroed `distribution` into the voxel and only sets `FIT_FAILED`. `pixel_selector.cpp:31` then emits `dist.true_signal_estimate` — zero — as the stacked pixel. Every stack in which a channel receives fewer than three frames silently outputs zeros (confirmed empirically by Task 20: n=1→0.0, n=2→0.0, n=3→correct). Three Phase B integration cases hit it with one synthetic frame per filter; real users hit it with any one- or two-frame stack. A KDE genuinely needs three samples for a bandwidth, but a one- or two-sample channel still has a value: the robust location the selector already computes (`biweight_location`: the sample itself for n=1, the biweight of the pair for n=2). The fix returns that as the estimate while keeping `converged = false` so the voxel is still flagged loudly.

**Files:**
- Modify: `src/lib/fitting/src/kde_fitter.cpp:125-131` (the `n < 3` branch)
- Modify: `src/lib/fitting/include/nukex/fitting/kde_fitter.hpp` (doc comment on `fit` describing the `n < 3` contract)
- Modify: `test/unit/fitting/test_kde_fitter.cpp`, `test/unit/fitting/test_model_selector.cpp`
- Check, and modify only if it has its own small-n guard that zeroes: `src/lib/gpu/src/` fitting kernels (`grep -rn "< 3" src/lib/gpu/`)

**Interfaces:**
- Consumes: `biweight_location(const float*, int)`, `mad(const float*, int)` from `nukex/fitting/robust_stats.hpp`; `FitResult`, `ZDistribution`, `DistributionShape`, `VoxelFlags::FIT_FAILED`.
- Produces: unchanged signatures. New contract: for `n < 3`, `fit()` returns `converged = false`, `n_samples = n`, `n_params = 1`, and a `distribution` with `shape = UNKNOWN`, `used_nonparametric = true`, `true_signal_estimate = kde_mode = robust_location`, `kde_bandwidth = 0`, `signal_uncertainty = robust_scale`, `confidence = 0`.

- [ ] **Step 1: Write the failing tests**

Append to `test/unit/fitting/test_kde_fitter.cpp` (add `#include "nukex/fitting/robust_stats.hpp"` and `#include "nukex/core/distribution.hpp"` if not already included):

```cpp
TEST_CASE("KDEFitter: n=1 returns the sample as the estimate, not zero", "[kde]") {
    float v[] = {0.37f};
    float w[] = {1.0f};
    KDEFitter kde;
    auto r = kde.fit(v, w, 1, biweight_location(v, 1), 0.0f);
    REQUIRE_FALSE(r.converged);
    REQUIRE(r.n_samples == 1);
    REQUIRE(r.distribution.true_signal_estimate == Catch::Approx(0.37f));
    REQUIRE(r.distribution.kde_mode             == Catch::Approx(0.37f));
    REQUIRE(r.distribution.signal_uncertainty   == Catch::Approx(0.0f));
    REQUIRE(r.distribution.confidence           == 0.0f);
    REQUIRE(r.distribution.used_nonparametric);
    REQUIRE(r.distribution.shape == DistributionShape::UNKNOWN);
}

TEST_CASE("KDEFitter: n=2 estimate is the robust location of the pair", "[kde]") {
    float v[] = {0.30f, 0.34f};
    float w[] = {1.0f, 1.0f};
    float rl = biweight_location(v, 2);
    float rs = mad(v, 2) * 1.4826f;
    KDEFitter kde;
    auto r = kde.fit(v, w, 2, rl, rs);
    REQUIRE_FALSE(r.converged);
    REQUIRE(r.n_samples == 2);
    REQUIRE(r.distribution.true_signal_estimate == Catch::Approx(rl));
    REQUIRE(r.distribution.true_signal_estimate == Catch::Approx(0.32f).margin(0.01f));
    REQUIRE(r.distribution.signal_uncertainty   == Catch::Approx(rs));
    REQUIRE(r.distribution.used_nonparametric);
}

TEST_CASE("KDEFitter: n=3 still runs the real KDE", "[kde]") {
    float v[] = {0.30f, 0.32f, 0.34f};
    float w[] = {1.0f, 1.0f, 1.0f};
    KDEFitter kde;
    auto r = kde.fit(v, w, 3, biweight_location(v, 3), mad(v, 3) * 1.4826f);
    REQUIRE(r.converged);
    REQUIRE(r.distribution.used_nonparametric);
    REQUIRE(r.distribution.true_signal_estimate == Catch::Approx(0.32f).margin(0.02f));
}
```

Append to `test/unit/fitting/test_model_selector.cpp`, constructing and inspecting the voxel exactly the way the file's existing `select(...)` cases do (same voxel type, same flag accessor):

```cpp
TEST_CASE("ModelSelector::select: a single sample sets FIT_FAILED but keeps the sample as the estimate",
          "[selector]") {
    float v[] = {0.42f};
    float w[] = {1.0f};
    // voxel setup: copy the pattern used by the existing select() tests in this file
    ModelSelector selector;
    selector.select(v, w, 1, voxel, 0);
    REQUIRE(voxel.has_flag(VoxelFlags::FIT_FAILED));                 // use the file's existing flag accessor name
    REQUIRE(voxel.distribution[0].true_signal_estimate == Catch::Approx(0.42f));
    REQUIRE(voxel.distribution[0].shape == DistributionShape::UNKNOWN);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cd build && make test_kde test_model_selector 2>&1 | grep -E "error" ; ./test/test_kde 2>&1 | tail -3; ./test/test_model_selector 2>&1 | tail -3`
Expected: the n=1 and n=2 KDE cases and the selector case fail on `true_signal_estimate == 0` (and `used_nonparametric == false`); the n=3 case passes already.

- [ ] **Step 3: Implement**

Replace the `n < 3` branch in `src/lib/fitting/src/kde_fitter.cpp` with:

```cpp
    if (n < 3) {
        // A KDE needs at least three samples for a bandwidth. One or two
        // frames give no distribution to fit, but they do give a value: the
        // robust location the selector already computed (the sample itself
        // for n == 1, the biweight of the pair for n == 2). Returning a
        // default-constructed distribution here silently zeroed every
        // stacked pixel fed by fewer than three frames. converged stays
        // false so ModelSelector::select flags the voxel FIT_FAILED; the
        // estimate is the data's own, never zero.
        result.converged = false;
        result.distribution.shape                = DistributionShape::UNKNOWN;
        result.distribution.used_nonparametric   = true;
        result.distribution.true_signal_estimate = static_cast<float>(robust_location);
        result.distribution.kde_mode             = static_cast<float>(robust_location);
        result.distribution.kde_bandwidth        = 0.0f;
        result.distribution.signal_uncertainty   = static_cast<float>(robust_scale);
        result.distribution.confidence           = 0.0f;
        return result;
    }
```

In `kde_fitter.hpp`, above `fit(`, add a doc comment: `/// n < 3: no KDE is possible; returns converged=false with true_signal_estimate = robust_location and signal_uncertainty = robust_scale (shape UNKNOWN, used_nonparametric). Callers keep the FIT_FAILED flag as the loud signal; the value is never zeroed.`

- [ ] **Step 4: GPU parity check**

Run: `grep -rn "< 3\|<3\|n_samples\b" src/lib/gpu/src/*.cpp src/lib/gpu/kernels/* 2>/dev/null | head`. If a GPU fitting kernel has its own small-sample guard that writes a zero estimate, apply the same rule there (estimate = robust location, uncertainty = robust scale) and note it in the report; if the GPU path only mirrors CPU-computed distributions (shadow buffers) or has no such guard, say so explicitly in the report with the grep output. `ctest -R gpu_agreement` must pass either way.

- [ ] **Step 5: Verify**

Run:
```bash
cd build && make -j$(nproc) 2>&1 | grep -E "error" ; ctest 2>&1 | tail -2
./test/integration/test_phase_a_router "[integration]" 2>&1 | tail -2
./test/integration/test_phase_b_qsolve  "[integration]" 2>&1 | tail -3
```
Expected: `100% tests passed, 0 tests failed out of 66`; phase_a `All tests passed (… 5 test cases)`; phase_b: 4 passed, 1 skipped (the still-gated negative-emission case — Task 20's resume un-gates it), 0 failed.

- [ ] **Step 6: Commit**

```bash
git add src/lib/fitting test/unit/fitting
git commit -m "$(cat <<'EOF'
fix(fitting): n < 3 keeps the sample's robust location instead of zeroing the voxel

KDEFitter::fit returned a default-constructed FitResult below three
samples; ModelSelector::select copied its zeroed distribution into the
voxel and pixel_selector emitted 0.0 as the stacked value. Any channel
fed by one or two frames stacked to zero, silently, with only the
FIT_FAILED flag set. The n < 3 path now returns the robust location the
selector already computed (the sample for n=1, the biweight of the pair
for n=2) as true_signal_estimate with the robust scale as uncertainty,
shape UNKNOWN, used_nonparametric, converged still false so the voxel
stays flagged. Surfaced by the Phase B integration tests (Task 20), which
stack one synthetic frame per filter.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01NqDedRCQxJy8EbbzWDKMFR
EOF
)"
```
