## Build & Release Workflow (MUST FOLLOW)

**IMPORTANT: Follow these steps for EVERY build/release:**

1. **NEVER install locally** - User will download from GitHub
2. **ALWAYS bump version number** before building:
   - Edit `src/NukeXModule.cpp`
   - Increment `MODULE_VERSION_BUILD` (or MINOR/MAJOR for significant changes)
   - Update `MODULE_RELEASE_YEAR/MONTH/DAY` to today's date
3. **Build**: `make clean && make`
4. **Sign**: `/opt/PixInsight/bin/PixInsight.sh --sign-module-file=NukeX-pxm.so --xssk-file=/home/scarter4work/projects/keys/scarter4work_keys.xssk --xssk-password="***REDACTED***"`
5. **Commit & Push to GitHub**: User downloads from https://github.com/scarter4work/NukeX
6. **DO NOT run `make install`** - this causes version conflicts

**Current Version Format**: `MAJOR.MINOR.REVISION.BUILD` (e.g., 1.1.0.1)

**ML Backup**: Full ML segmentation code is preserved in `/home/scarter4work/projects/NukeX2/` (local only, not on GitHub). Use this if re-adding ML features.

---

## Training Optimization

- **GPU**: NVIDIA GeForce RTX 5070 Ti (16GB VRAM)
- **Recommendation**: Use `--batch-size 32` or `--batch-size 64` for training to better utilize GPU memory
- Current batch-size 16 only uses ~4GB of 16GB available VRAM
- Larger batch sizes = faster training throughput

---

## Intelligent Pixel Selection System - Refactor Plan

### Overview

The goal is to create an intelligent stacking system that **SELECTS** the statistically best pixel value from a stack of prestretched subframes, using both statistical analysis and ML-based semantic understanding.

**Key Insight**: The distribution fitted across the stack at each pixel position serves dual purposes:
1. **Classification** - What type of astronomical feature is this pixel?
2. **Selection** - Which frame's value is best for this type of pixel?

### Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                    PRESTRETCHED SUBFRAME STACK                      │
│                     Frame 1, 2, 3, ... N                            │
└─────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────────┐
│                   ML SEGMENTATION (21 classes)                      │
│  Star core, star halo, bright emission, faint emission, dark       │
│  nebula, reflection nebula, galaxy core, galaxy arm, dust lane,    │
│  background, transition zones, etc.                                 │
└─────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────────┐
│              PER-PIXEL DISTRIBUTION FITTING                         │
│  For each (x,y): fit distribution across N frames                   │
│  → Statistical signature + outlier detection                        │
└─────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────────┐
│              ML-BASED PIXEL SELECTOR                                │
│  Input: distribution params, ML class, spatial context,            │
│         FITS metadata (target object), N candidate values           │
│  Output: selected value, confidence, source frame                   │
└─────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────────┐
│              TRANSITION CHECKER (tile-based)                        │
│  Detect hard transitions between regions in final image             │
│  Smooth/blend if discontinuities found                              │
└─────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────────┐
│                    FINAL INTEGRATED IMAGE                           │
└─────────────────────────────────────────────────────────────────────┘
```

### Components to Implement

#### 1. PixelStackAnalyzer (refactor from DistributionFitter)

**Purpose**: Analyze pixel values across the stack at each (x,y) position

**Input**: N prestretched frames

**Output per pixel**:
```cpp
struct PixelStackMetadata {
   DistributionParams dist;      // type, μ, σ, skewness, kurtosis
   float selectedValue;          // The chosen "best" value
   uint16_t sourceFrame;         // Which frame contributed this pixel
   float confidence;             // Confidence in selection
   uint8_t outlierMask;          // Bitmask of which frames were outliers
};
```

**Key behavior**:
- Fit distribution (Gaussian, Lognormal, Skewed) to N values at each pixel
- Distribution shape informs pixel classification
- Identify statistical outliers based on distribution

#### 2. SegmentationIntegration

**Purpose**: Connect to existing 21-class ML segmentation model

**Provides**:
- Per-pixel class labels (star core, nebula, background, etc.)
- Class probabilities for uncertain regions
- Spatial context (what are neighboring pixels classified as?)

**Integration points**:
- Run on reference frame or median stack
- Classes inform selection strategy in PixelSelector
- FITS metadata (OBJECT keyword) can weight expected classes (M42 → expect emission nebula)

#### 3. PixelSelector

**Purpose**: Make the final decision on which pixel value to use

**Inputs**:
- Distribution params from PixelStackAnalyzer
- ML class from SegmentationIntegration
- Spatial context (neighboring pixels' classes and selections)
- FITS metadata (target object, exposure time, etc.)
- All N candidate values

**Selection logic varies by class**:
| Class | Selection Strategy |
|-------|-------------------|
| Background | Select near median, reject high outliers (gradients) |
| Faint emission | Preserve signal, reject contamination |
| Bright emission | Favor signal, careful with saturation |
| Dark nebula | Preserve LOW values, don't reject as outliers |
| Star core | Don't reject high values, reject LOW outliers |
| Star halo | Preserve gradients, spatial consistency |
| Transition zones | Consider neighbors, smooth selection |

**Future**: Train non-linear ML model to learn optimal selection from examples

#### 4. TransitionChecker (tile-based post-processing)

**Purpose**: Detect and smooth hard transitions in final integrated image

**When to use**: After pixel selection, before final output

**Algorithm**:
1. Divide final image into tiles (16x16 or configurable)
2. For each tile boundary, check for discontinuities:
   - Compare edge pixels across tile boundaries
   - Look for sudden jumps in value or gradient
3. If hard transition detected:
   - Check if it aligns with real feature (star edge, nebula boundary)
   - If artifact: apply localized smoothing/blending
   - If real: preserve

**Metrics**:
- Gradient magnitude at tile boundaries
- Statistical difference between adjacent tiles
- Correlation with segmentation boundaries (real vs artifact)

### Per-Pixel Metadata Storage

For a 4K image (4096 x 4096) with full per-pixel metadata:
- DistributionParams: ~32 bytes (type + 4 doubles)
- Selected value: 4 bytes
- Source frame: 2 bytes
- Confidence: 4 bytes
- Total: ~42 bytes/pixel → ~700 MB for 4K

**Storage options**:
1. Full per-pixel (most accurate, large)
2. Compressed (store deltas, quantize confidence)
3. Hybrid (full for signal regions, sparse for background)

### Implementation Order

1. **PixelStackAnalyzer** - Core refactor of DistributionFitter
   - Work on pixel stacks instead of spatial tiles
   - Per-pixel distribution fitting

2. **Basic PixelSelector** - Rule-based initial version
   - Simple class-based selection rules
   - Hooks for ML integration

3. **SegmentationIntegration** - Connect existing 21-class model
   - Wire up inference
   - Class-to-strategy mapping

4. **TransitionChecker** - Post-processing smoothing
   - Reuse tile-based infrastructure
   - Boundary detection and smoothing

5. **ML PixelSelector** - Train learned selection model
   - Generate training data from manual selections
   - Non-linear model for complex decisions

### Files to Create/Modify

**New files**:
- `src/engine/PixelStackAnalyzer.h/.cpp` - Per-pixel distribution fitting
- `src/engine/PixelSelector.h/.cpp` - Selection decision maker
- `src/engine/TransitionChecker.h/.cpp` - Post-integration smoothing

**Modify**:
- `src/engine/DistributionFitter.h/.cpp` - Repurpose for TransitionChecker
- `src/engine/Segmentation.h/.cpp` - Add integration hooks
- `CMakeLists.txt` / `Makefile` - Add new sources

### Example: Why This Matters

**Scenario**: M42 subframe stack, pixel at Trapezium region

```
Frame 1: 0.82  (good)
Frame 2: 0.79  (good, slight seeing)
Frame 3: 0.31  (cloud passed)
Frame 4: 0.84  (good)
Frame 5: 0.80  (good)

Distribution: μ=0.81, σ=0.02 (excluding outlier)
ML Class: "Bright emission nebula"
Selection: Frame 4 (0.84) - highest quality signal
Rejected: Frame 3 - 25σ outlier, cloud contamination
```

**Without ML class**: Might incorrectly average or select median
**With ML class**: Knows this is bright nebula, favors strong signal, rejects low outlier

