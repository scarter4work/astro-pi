# NukeX - Intelligent Astrophotography Processing for PixInsight

> "Will blow your socks off!"

## Project Overview

NukeX is a PixInsight module with two main processes:

1. **NukeX** - Region-aware image stretching using ML segmentation
2. **NukeXStack** - Intelligent pixel selection for subframe integration *(PRIMARY FOCUS)*

The stacking functionality (NukeXStack) is the priority. It uses statistical distribution fitting and ML-guided selection to choose the optimal pixel value from a stack of prestretched subframes.

---

## Architecture: NukeXStack (Intelligent Stacking)

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

---

## Current Implementation Status

### Core Stacking Components (IMPLEMENTED)

| Component | File | Status | Description |
|-----------|------|--------|-------------|
| PixelStackAnalyzer | `engine/PixelStackAnalyzer.cpp` | Complete | Per-pixel distribution fitting across stack |
| PixelSelector | `engine/PixelSelector.cpp` | Complete | ML-guided pixel selection with class strategies |
| TransitionChecker | `engine/TransitionChecker.cpp` | Complete | Tile-based transition smoothing |
| DistributionFitter | `engine/DistributionFitter.cpp` | Complete | Statistical distribution fitting |
| Segmentation | `engine/Segmentation.cpp` | Complete | ONNX-based 21-class segmentation |

### Process Implementation (IMPLEMENTED)

| Component | File | Status | Description |
|-----------|------|--------|-------------|
| NukeXStackProcess | `NukeXStackProcess.cpp` | Complete | MetaProcess definition |
| NukeXStackInstance | `NukeXStackInstance.cpp` | Complete | Full integration pipeline |
| NukeXStackInterface | `NukeXStackInterface.cpp` | Complete | User interface |
| NukeXStackParameters | `NukeXStackParameters.cpp` | Complete | All parameters defined |

### Class-Based Selection Strategies

| Class | Selection Strategy |
|-------|-------------------|
| Background | Select near median, reject high outliers (gradients, satellites) |
| Faint emission | Preserve signal, reject contamination |
| Bright emission | Favor signal, careful with saturation |
| Dark nebula | Preserve LOW values, don't reject as outliers |
| Star core | Don't reject high values, reject LOW outliers |
| Star halo | Preserve gradients, spatial consistency |
| Transition zones | Consider neighbors, smooth selection |

---

## What Needs To Be Done

### 1. Build System
- [ ] Create Makefile for Linux build
- [ ] Verify PCL include paths
- [ ] Set up ONNX Runtime linking
- [ ] Test build compiles without errors

### 2. Module Loading
- [ ] Verify static initializer fix in NukeXModule.cpp
- [ ] Test module loads in PixInsight
- [ ] Debug if module still doesn't appear

### 3. Testing
- [ ] Test with real subframe stack
- [ ] Verify segmentation works (or gracefully falls back)
- [ ] Check pixel selection produces sensible results
- [ ] Validate transition smoothing

### 4. PixInsight Update Repository
- [ ] Create repository structure on GitHub
- [ ] Build signed module
- [ ] Create updates.xri manifest
- [ ] Sign manifest
- [ ] Test installation via PI updater

---

## Build Instructions

### Requirements
- PixInsight Core 1.8.9+
- PCL SDK
- C++17 compiler (g++ 9+)
- ONNX Runtime 1.14+ (optional, for ML segmentation)

### Linux Build
```bash
cd /home/scarter4work/projects/NukeX2

# Build (once Makefile is created)
make clean && make -j8

# Sign the module
/opt/PixInsight/bin/PixInsight.sh --sign-module-file=NukeX-pxm.so \
  --xssk-file=/home/scarter4work/projects/keys/scarter4work_keys.xssk \
  --xssk-password="***REDACTED***"

# Install
sudo cp NukeX-pxm.so NukeX-pxm.xsgn /opt/PixInsight/bin/
```

---

## Training Information

Training data is stored in `training/` (44GB, excluded from git).

- **GPU**: NVIDIA GeForce RTX 5070 Ti (16GB VRAM)
- **Batch size**: 32 or 64 recommended (current 16 only uses ~4GB)
- **Model**: 21-class segmentation for astrophotography features

---

## File Structure

```
src/
├── NukeXModule.cpp           # Module entry (both processes)
├── NukeXProcess.*            # Single-image stretch process
├── NukeXInstance.*           # Stretch execution
├── NukeXInterface.*          # Stretch UI
├── NukeXParameters.*         # Stretch parameters
├── NukeXStackProcess.*       # Stacking process (PRIORITY)
├── NukeXStackInstance.*      # Stack integration execution
├── NukeXStackInterface.*     # Stacking UI
├── NukeXStackParameters.*    # Stacking parameters
└── engine/
    ├── PixelStackAnalyzer.*  # Per-pixel distribution fitting
    ├── PixelSelector.*       # ML-guided pixel selection
    ├── TransitionChecker.*   # Transition smoothing
    ├── DistributionFitter.*  # Statistical distributions
    ├── Segmentation.*        # ONNX segmentation
    ├── StretchLibrary.*      # 11 stretch algorithms
    ├── SelectionRules.*      # Per-region algorithm selection
    ├── Compositor.*          # Regional stretch blending
    └── algorithms/           # Individual stretch implementations
```

---

## GitHub Repository

- **Repository**: https://github.com/scarter4work/NukeX2
- **Update Repository**: To be set up at same location

---

## Why This Approach Works

**Traditional stacking** (median, sigma clip, etc.) treats all pixels the same.

**NukeXStack understands context:**

Example: M42 Trapezium region
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

---

*Document Version: 2.0*
*Consolidated: January 2026*
*Project: NukeX / NukeXStack*
