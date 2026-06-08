# Phase 5: lib/stretch — Stretch Pipeline with 11 Scientific Stretch Operations

*Design validated 2026-03-31*
*References: nukex_additions_spec.md (Section 3), implementation design (Section 8)*

---

## 1. Scope

Build the complete stretch pipeline: StretchOp base class, StretchPipeline container,
advisory ordering system, quick_preview_stretch, and all 11 stretch operations. Each
operation is scientifically verified against its published reference with known-answer
tests and visual validation on real M16 astrophotography data.

Visual validation uses PNG output (via stb_image_write) for user review. FITS output
is deferred to a later phase.

**Critical rules from v3 failure:**
- Every formula verified against its published reference
- Every op tested with minimum 10 known-answer assertions
- Visual output on real data for user evaluation
- No shortcuts, no stubs, no TODOs
- Double-check all math: "Am I sure?"

---

## 2. Dependencies

```
lib/stretch depends on:
  - lib/core (types)
  - lib/io (Image)
  - lib/fitting (robust_stats — for SAS tile median computation)

Test utilities:
  - stb_image_write (vendored, header-only) for PNG output
  - Real FITS test data at /home/scarter4work/projects/processing/M16/
```

---

## 3. Architecture

### 3.1 StretchOp Base Class

```cpp
enum class StretchCategory { PRIMARY, SECONDARY, FINISHER };

class StretchOp {
public:
    bool            enabled  = false;
    int             position = 0;
    std::string     name;
    StretchCategory category = StretchCategory::PRIMARY;

    virtual ~StretchOp() = default;

    // Apply stretch to image in-place. Image is float32, [0,1].
    virtual void apply(Image& img) const = 0;

    // Apply to a single float value (for LUT pre-computation, scalar ops only).
    // Spatially-varying ops (SAS) may not implement this.
    virtual float apply_scalar(float x) const { return x; }
};
```

### 3.2 StretchPipeline

```cpp
class StretchPipeline {
public:
    std::vector<std::unique_ptr<StretchOp>> ops;

    void execute(Image& img) const;

    // Advisory ordering warnings (non-blocking).
    // Returns warning messages if ordering is suboptimal.
    std::vector<std::string> check_ordering() const;

    // Quick preview — arcsinh on a COPY, never modifies working data.
    static Image quick_preview_stretch(const Image& linear_src, float alpha = 500.f);
};
```

### 3.3 Luminance-Only Mode

Many ops support a `luminance_only` flag. When true on RGB data:
1. Compute luminance: `L = 0.2126 R + 0.7152 G + 0.0722 B` (Rec. 709)
2. Apply stretch to L: `L' = stretch(L)`
3. Scale RGB by ratio: `R' = R × (L'/L)`, same for G, B
4. Clamp to [0, 1]

This preserves chromaticity (hue + saturation) while stretching brightness. Used by
ArcSinh, GHS, Histogram, Log, SAS, Veralux, OTS.

Implemented as a shared utility function in the base, not duplicated per op:

```cpp
// In stretch_utils.hpp
void apply_luminance_only(Image& img, const std::function<float(float)>& scalar_fn);
```

---

## 4. The 11 Stretch Operations

### 4.0 MTF (Midtone Transfer Function)
- **Category:** FINISHER
- **Parameters:** midtone (m), shadows (c0), highlights (c1), luminance_only
- **Formula:** `f(x) = ((m-1)·xc) / ((2m-1)·xc - m)` where `xc = clamp((x-c0)/(c1-c0), 0, 1)`
- **Verification:** apply(0)=0, apply(1)=1, apply(m)=0.5
- **Reference:** PixInsight STF (Vizier)

### 4.1 Histogram (CDF Equalization)
- **Category:** SECONDARY
- **Parameters:** clip_fraction, luminance_only, n_bins (4096)
- **Algorithm:** Build CDF from histogram, clip extremes, normalize to [0,1] mapping
- **Verification:** Monotonic, output CDF approximately uniform
- **Reference:** Standard CDF equalization

### 4.2 GHS (Generalized Hyperbolic Stretch)
- **Category:** PRIMARY
- **Parameters:** D (intensity), b (hyperbolic param), SP (symmetry point), LP, HP, luminance_only
- **Formula:** See additions spec Section 3, Op 2
- **Verification:** Reduces to arcsinh when b→∞, identity when D=0, apply(0)=0, apply(1)=1
- **Reference:** Gharat & Treweek (2021), JOSS

### 4.3 ArcSinh
- **Category:** PRIMARY
- **Parameters:** alpha (stretch factor), luminance_only
- **Formula:** `f(x) = arcsinh(α·x) / arcsinh(α)`
- **Verification:** apply(0)=0, apply(1)=1, hue-preserving in luminance mode
- **Reference:** Lupton et al. (1999)

### 4.4 Log
- **Category:** PRIMARY
- **Parameters:** alpha, luminance_only
- **Formula:** `f(x) = log1p(α·x) / log1p(α)`
- **Verification:** apply(0)=0, apply(1)=1
- **Reference:** Standard log1p stretch

### 4.5 Lumpton (Lupton et al. 2004)
- **Category:** PRIMARY
- **Parameters:** alpha, beta (softening), channel_mode (JOINT_RGB, PER_CHANNEL, MONO)
- **Formula (joint RGB):** `factor = arcsinh(α·I+β) / ((α·I+β)·arcsinh(α))` where `I = (R+G+B)/3`
- **Verification:** Hue preserved (R/G, G/B ratios unchanged), factor→1 as I→0
- **Reference:** Lupton et al. (2004), PASP 116, 133

### 4.6 RNC (Roger N. Clark)
- **Category:** SECONDARY
- **Parameters:** black_point, white_point, gamma, channel_mode
- **Formula:** `f(x) = pow(clamp((x-bp)/(wp-bp), 0, 1), 1/γ)`
- **Verification:** Pure power law, apply(bp)=0, apply(wp)=1
- **Reference:** Roger N. Clark methodology

### 4.7 Photometric
- **Category:** SECONDARY
- **Parameters:** mag_zero_point, display_min_mag, display_max_mag, flux_scale
- **Formula:** `mag = MAGZERO - 2.5·log10(flux)`, then linear remap to [0,1]
- **Verification:** Flux-preserving, refuses without MAGZERO
- **Reference:** Pogson magnitude scale

### 4.8 OTS (Optimal Transport Stretch)
- **Category:** SECONDARY
- **Parameters:** n_bins (2048), luminance_only, target_distribution (Munsell/sqrt/uniform/Gaussian)
- **Algorithm:** Compute source CDF, compute target CDF, build monotonic LUT via quantile matching
- **Verification:** Monotonic output, target CDF approximately matched
- **Reference:** Villani (2003) — 1D OT reduces to quantile matching

### 4.9 SAS (Signal-Adaptive Stretch)
- **Category:** PRIMARY
- **Parameters:** tile_size (256), tile_overlap (0.5), target_median (0.25), max_stretch, min_stretch, luminance_only
- **Algorithm:** Per-tile GHS fitting → tile stretch maps → Gaussian-weighted blending
- **Verification:** Smooth tile boundaries, local adaptation visible
- **Reference:** Spec-defined (novel). Per-tile GHS from Gharat & Treweek (2021)

### 4.10 Veralux (Filmic Tone Curve)
- **Category:** PRIMARY
- **Parameters:** exposure, contrast, toeStrength, shoulderStrength, blackPoint, whitePoint, luminance_only
- **Algorithm:** Exposure scale → remap → toe quadratic → linear mid → shoulder quadratic
- **Verification:** C1 continuity at transitions, smooth S-curve, apply(0)=0, apply(1)=1
- **Reference:** Hable (2010) / Narkowicz (2015) filmic tone mapping, adapted for astro

---

## 5. File Structure

```
third_party/stb/
├── stb_image_write.h      Vendored header-only PNG writer
├── LICENSE                 Public domain

src/lib/stretch/
├── CMakeLists.txt
├── include/
│   └── nukex/
│       └── stretch/
│           ├── stretch_op.hpp           Base class + category enum
│           ├── stretch_pipeline.hpp     Pipeline container + advisory ordering
│           ├── stretch_utils.hpp        Luminance-only helper, RGB↔L conversion
│           ├── mtf_stretch.hpp          Op 0
│           ├── histogram_stretch.hpp    Op 1
│           ├── ghs_stretch.hpp          Op 2
│           ├── arcsinh_stretch.hpp      Op 3
│           ├── log_stretch.hpp          Op 4
│           ├── lumpton_stretch.hpp      Op 5
│           ├── rnc_stretch.hpp          Op 6
│           ├── photometric_stretch.hpp  Op 7
│           ├── ots_stretch.hpp          Op 8
│           ├── sas_stretch.hpp          Op 9
│           └── veralux_stretch.hpp      Op 10
├── src/
│   ├── stretch_pipeline.cpp
│   ├── stretch_utils.cpp
│   ├── mtf_stretch.cpp
│   ├── histogram_stretch.cpp
│   ├── ghs_stretch.cpp
│   ├── arcsinh_stretch.cpp
│   ├── log_stretch.cpp
│   ├── lumpton_stretch.cpp
│   ├── rnc_stretch.cpp
│   ├── photometric_stretch.cpp
│   ├── ots_stretch.cpp
│   ├── sas_stretch.cpp
│   └── veralux_stretch.cpp

test/util/
├── png_writer.hpp           Test utility: write Image to 16-bit PNG
├── png_writer.cpp
├── test_data_loader.hpp     Test utility: load M16 frame, debayer, return Image
└── test_data_loader.cpp

test/unit/stretch/
├── test_stretch_pipeline.cpp
├── test_mtf.cpp
├── test_histogram.cpp
├── test_ghs.cpp
├── test_arcsinh.cpp
├── test_log.cpp
├── test_lumpton.cpp
├── test_rnc.cpp
├── test_photometric.cpp
├── test_ots.cpp
├── test_sas.cpp
└── test_veralux.cpp

test/output/                  PNG visual output directory (gitignored)
```

---

## 6. Implementation Batches

### Phase 5A: Infrastructure + Tier 3 (simple scalar ops)
1. Vendor stb_image_write
2. PNG writer test utility
3. Test data loader (M16 frame → debayered Image)
4. StretchOp base + StretchPipeline + stretch_utils (luminance-only helper)
5. MTF, ArcSinh, Log, RNC
6. Visual evaluation: 4 PNGs for user review

### Phase 5B: Tier 2 (medium complexity)
7. GHS
8. Veralux
9. Photometric
10. Histogram
11. Visual evaluation: 4 more PNGs

### Phase 5C: Tier 1 (complex / novel)
12. Lumpton (joint RGB)
13. OTS (optimal transport)
14. SAS (signal-adaptive)
15. Visual evaluation: 3 more PNGs

### Phase 5D: Integration + advisory ordering
16. Advisory ordering warnings
17. Full pipeline integration test (chain multiple ops)
18. Visual evaluation: combined pipeline PNG

---

## 7. Testing Strategy

### Known-answer tests (per op, minimum 10 assertions)
- `apply_scalar(0.0) == 0.0` (black stays black)
- `apply_scalar(1.0) == 1.0` (white stays white, with default params)
- `apply_scalar(midpoint)` matches hand-computed value
- Monotonicity: for x₁ < x₂, `apply_scalar(x₁) <= apply_scalar(x₂)`
- Identity case: verify parameters that produce identity transform
- Boundary cases: x slightly above 0, x slightly below 1
- Specific numerical values verified by hand calculation

### Visual output tests (per op)
- Load M16 debayered frame
- Apply stretch with default parameters
- Write to `test/output/stretch_<name>.png`
- User evaluates: faint structure visible? Stars not blown? Smooth gradients?

### Cross-validation (where possible)
- Compare apply_scalar output against Python numpy reference for 5+ test points
- Document the Python verification in test comments

---

## 8. References

| Op | Reference |
|----|-----------|
| MTF | PixInsight STF (Vizier) |
| Histogram | Standard CDF equalization |
| GHS | Gharat & Treweek (2021), JOSS |
| ArcSinh | Lupton et al. (1999) |
| Log | Standard log1p |
| Lumpton | Lupton et al. (2004), PASP 116, 133 |
| RNC | Roger N. Clark methodology |
| Photometric | Pogson magnitude scale |
| OTS | Villani (2003), optimal transport |
| SAS | Spec-defined (novel), per-tile GHS from Gharat & Treweek |
| Veralux | Hable (2010) / Narkowicz (2015) filmic tone mapping |

---

*End of Phase 5 design specification*
