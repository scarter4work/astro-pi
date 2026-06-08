# NukeX v3 — C++ Library Research

*March 5, 2026 — Comprehensive survey of matrix, statistical, and numerical libraries*

---

## Selected Stack

| Layer | Library | Version | License | Header-Only |
|---|---|---|---|---|
| 3D Tensor | xtensor | 0.26.x (C++17) | BSD-3 | Yes |
| SIMD | xsimd | latest | BSD-3 | Yes |
| Distributions | Boost.Math | 1.89+ | BSL | Yes |
| Optimizer | LBFGSpp | latest | MIT | Yes |
| Small vectors | Eigen | 5.x | MPL-2.0 | Yes |
| Robust stats | PCL native | — | PCLL | Already linked |

All licenses are maximally permissive for commercial PixInsight module distribution.

---

## Matrix / Tensor Libraries Evaluated

### xtensor 0.26.x (SELECTED)
- Native N-dimensional arrays, NumPy-like API
- **Critical advantage**: layout control — store cube as (N_subs, H, W) row-major → Z-values contiguous
- BSD-3 license, header-only, C++17 compatible
- SIMD via xsimd (same ecosystem)
- `xt::view(cube, xt::all(), x, y)` extracts contiguous Z-column

### Armadillo 15.2 (Not selected)
- Best API ergonomics: `.tube(x,y)` directly extracts Z-column
- BUT: column-major only, Z-axis always strided
- Apache 2.0, very mature (15+ years)
- Eliminated because strided Z-access hurts the hot loop (16.7M extractions)

### Eigen 5.0 (Used only for LBFGSpp)
- Tensor module is "unsupported" — clunky `chip().chip()` for Z-extraction
- Core library excellent for 2D linear algebra
- MPL-2.0, header-only
- Used indirectly via LBFGSpp for optimizer's small parameter vectors

### Blaze 3.8 — ELIMINATED
- No releases since 2020, effectively abandoned
- No native 3D tensor support

### Fastor 0.6 — ELIMINATED
- Fixed-size stack-only tensors, cannot handle 32GB cube

### Intel MKL — ELIMINATED
- No macOS ARM support, not a tensor library (BLAS backend only)
- ISSL license (not standard OSS)

### OpenBLAS — NOT NEEDED
- BLAS backend, not a tensor library
- Our workload is per-column statistics, not matrix multiplication

---

## Statistical / Distribution Libraries Evaluated

### Boost.Math 1.89+ (SELECTED)
- 33+ distributions with PDF/CDF/quantile: Normal, Skew-Normal, Poisson, etc.
- KS test p-values via `kolmogorov_smirnov_distribution`
- Special functions: lgamma, digamma, erf, erfc
- BSL license, fully header-only, thread-safe (stateless functions)
- Does NOT provide: MLE fitting, AIC/BIC, outlier tests, EM

### LBFGSpp (SELECTED for optimizer)
- Header-only L-BFGS and L-BFGS-B optimizer
- MIT license, depends on Eigen
- Minimal per-call overhead: O(N*M) per iteration, N=3 params, M=3-5 corrections
- Thread-safe with per-thread solver objects
- Perfect for Skew-Normal MLE (3 parameters)

### dlib (Strong alternative, not selected)
- BSL license, `find_min()` with BFGS/L-BFGS/BOBYQA
- Heavier than LBFGSpp for small-parameter problems
- Would be the choice if we needed derivative-free optimization (BOBYQA)

### GSL — ELIMINATED (GPL license)
### Ceres — ELIMINATED (overkill, heavy deps, per-problem overhead at 67M calls)
### Stan Math — ELIMINATED (autodiff tape overhead, massive dependency tree)
### NLopt — ELIMINATED (LGPL requires dynamic linking)
### ALGLIB — ELIMINATED (GPL free / paid commercial)
### kthohr/stats — ELIMINATED (no Skew-Normal, stale maintenance, Boost.Math is superior)

---

## Per-Distribution MLE Strategy

### Closed-Form (no optimization needed)
- **Gaussian** (k=2): mu = mean(x), sigma² = (1/n)*sum((x-mu)²) — ~1 µs/pixel
- **Poisson** (k=1): lambda = mean(x) — ~0.5 µs/pixel

### Iterative (optimization required)
- **Skew-Normal** (k=3): L-BFGS via LBFGSpp, ~10-20 iterations — ~20-50 µs/pixel
  - Initialize: xi=mean, omega=stddev, alpha from sample skewness
  - Gradient: analytically derived from Boost.Math skew_normal PDF
- **Bimodal Mixture** (k=5): Hand-rolled 2-component EM, ~15-30 iterations — ~50-100 µs/pixel
  - Initialize: k-means on sorted data (split at median)
  - E-step: log-sum-exp trick for numerical stability
  - M-step: weighted mean/variance per component
  - Convergence: monitor log-likelihood delta

### Model Selection
- AIC = 2k - 2*logL
- BIC = k*ln(n) - 2*logL
- AICc = AIC + (2k(k+1))/(n-k-1) for small samples

### Performance Estimate
- Total per pixel: ~70-150 µs
- 16.7M pixels single-threaded: ~1.2-2.5 seconds
- 16-core parallel: ~0.1-0.2 seconds (embarrassingly parallel)

---

## What We Implement Ourselves (~500 lines)

### Trivial (< 10 lines each)
- AIC / BIC / AICc scoring
- Gaussian closed-form MLE
- Poisson closed-form MLE
- Chauvenet's criterion (uses Boost.Math normal CDF)

### Moderate (~50-100 lines each)
- Grubbs' test (uses Boost.Math Student's t quantile)
- Dixon's Q test (small lookup table for n ≤ 30)
- Generalized ESD (iterative Grubbs')
- Winsorized mean/variance
- 1D Gaussian KDE with Silverman bandwidth

### Substantial (~150-200 lines)
- 2-component Gaussian EM for bimodal mixture fitting
- Log-sum-exp utilities
- Skew-Normal MLE objective + gradient function

---

## Numerical Stability Notes

1. Always work in log-space (log-likelihood, not likelihood)
2. Log-sum-exp trick for mixture model likelihoods
3. Use lgamma instead of gamma (Boost.Math provides both)
4. Floor variance estimates in EM (min 1e-10) to prevent singularity
5. Transform bounded parameters for unconstrained optimization (log(sigma), softmax for weights)
6. Promote pixel data from float to double for all statistical computation
7. Use Kahan compensated summation for accumulations (PCL's StableSum available)

---

## PCL Native Statistics (Already Available)

- Median, TrimmedMean, OrderStatistic (percentiles)
- MAD, TwoSidedMAD, Sn, Qn (robust scale)
- BiweightMidvariance, BendMidvariance
- StableMean, StableSum, StableSumOfSquares (Kahan compensated)
- ImageStatistics (parallel, multi-threaded)

These are production-quality and should be used where applicable rather than reimplemented.

---

## Key Architectural Decision: Memory Layout

Store the 3D cube as `xt::xtensor<float, 3>` with shape `(N_subs, height, width)` in row-major layout.

This makes Z-values at position (x,y) contiguous in memory:
```
cube[0][y][x], cube[1][y][x], ..., cube[N-1][y][x]  ← contiguous floats
```

The hot loop extracts this contiguous slice via `xt::view(cube, xt::all(), y, x)` — no strided access, sequential cache line reads, SIMD-friendly.
