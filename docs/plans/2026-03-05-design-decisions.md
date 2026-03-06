# NukeX v3 — Design Decisions & Deferred Optimizations

*Captured from design session — March 5, 2026*

---

## Confirmed Design Decisions

### Scope
- **Two processes in one module**: NukeXStack (new paradigm) + NukeXStretch (ported, no ML)
- All ONNX/ML segmentation is **dropped entirely**
- 11 stretch algorithms ported as-is from v2 — self-contained, no region analysis dependency
- Auto-stretch mode: lightweight statistical heuristics for Phase 1

### Dependencies
- **Eigen** (header-only) — all matrix math, 3D cube representation
- **Boost.Math** — distribution fitting (MLE), AIC/BIC scoring, statistical functions
- **PCL/C++17** — PixInsight process module framework
- No ONNX Runtime dependency

### Memory Model
- **Full 3D cube in RAM** — load all subs into memory
- `xt::xtensor<float, 3, xt::layout_type::column_major>` with shape `(N_subs, height, width)`
- **Column-major layout** — makes the first axis (N_subs / Z) fastest-varying, so Z-values at each (y,x) are contiguous at stride 1. (Plan originally said row-major, which was wrong — row-major makes the *last* axis fastest.)
- No row-streaming or tiling for Phase 1
- Rationale: simplifies algorithm correctness; optimize later once proven
- Constraint: requires sufficient RAM (~32GB for 4K × 500 subs at float32)

### Distribution Fitting Strategy
- **Brute force all four models per pixel**: Gaussian, Skewed (Skew-Normal), Bimodal, Poisson
- AIC/BIC scoring to select best-fit model per pixel
- ~67M distribution fits for a 4K image — trivially parallelizable
- Each pixel is independent → PCL thread pool saturates all cores
- No tiered/shortcut fitting — correctness and simplicity first

### Reusable Parts from v2
- **FrameStreamer concept** — adapt for full-cube loading (read all frames into Eigen tensor)
- **FITS metadata extraction** — keyword parsing for FWHM, eccentricity, sky background, HFR, altitude, exposure, gain, CCD temp, object, filter
- **Build system** — CMakeLists.txt + Makefile dual build, adapted for Eigen/Boost deps
- **Release infrastructure** — updates.xri generation, tar.gz packaging, module signing
- **Module registration pattern** — MetaModule/MetaProcess/ProcessImplementation/ProcessInterface

### Stretch Auto-Selection (Phase 1)
- Global image statistics: histogram shape, dynamic range, median, noise floor
- Simple decision tree based on PCL ImageStatistics
- No spatial awareness in Phase 1

---

## Deferred to Phase 2

### Memory Optimization
- Row-stripe streaming with Eigen tile buffers (e.g., 64-row stripes)
- Adaptive tiling based on available RAM at runtime
- Evaluate: is row-streaming even needed if cube fits in RAM? Benchmark first.

### Tiered Distribution Fitting
- Start with Gaussian; only try others if goodness-of-fit exceeds threshold
- May not be needed if brute-force A is fast enough with full parallelization
- Profile first — branching may hurt vectorization more than it saves

### Distribution-Derived Segmentation
- **Key insight**: the per-pixel distribution fitting results from stacking ARE a segmentation map
  - Star cores → tight high-value Gaussian
  - Nebula → low-signal Poisson/Gaussian
  - Background → noise-dominated distributions
  - Satellite trails → skewed/bimodal in affected pixels
- Feed provenance map + distribution parameters into stretch auto-selection
- Per-pixel or per-region stretch informed by stacking engine output
- This replaces ML segmentation with mathematically derived regions

### SIMD / Vectorization
- Evaluate Eigen's SIMD support for the distribution fitting inner loop
- Consider explicit AVX2/AVX-512 for the hot path if Eigen's auto-vectorization is insufficient

### Companion Visualization App
- 3D point cloud per pixel showing Z distribution
- Accepted/rejected values color-coded
- Technology TBD (Electron, web, native)
- Reads provenance/distribution data from NukeX output files

---

## Open Questions (Revisit in Phase 2)
- Eigen tensor vs raw Eigen matrices for the 3D cube — memory layout implications (row-major vs column-major for cache line efficiency during per-pixel column traversal)
- Boost.Math distribution fitting hot-loop performance at 16.7M pixels — profile and evaluate
- Whether provenance map should be stored as FITS extension or separate XISF property
- PixInsight certified developer submission timeline
