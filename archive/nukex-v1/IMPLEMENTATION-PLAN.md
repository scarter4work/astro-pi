# NukeX Implementation Plan for Claude Code

> A step-by-step guide for implementing NukeX with Claude Code assistance

## Overview

This document outlines the implementation strategy for NukeX, an AI-driven region-aware stretch module for PixInsight. The plan is structured for iterative development with Claude Code, focusing on getting a working module first, then adding sophistication.

---

## Prerequisites

Before starting implementation:

### Environment Setup
- [ ] Install PixInsight (latest version)
- [ ] Download and install PCL SDK from https://pixinsight.com/developer/
- [ ] Install ONNX Runtime C++ (v1.16+)
- [ ] Set up build environment (g++ with C++17 support)
- [ ] Verify PCL sample module compiles and loads

### Required Paths (configure in Makefile)
```bash
PCL_DIR=/opt/PixInsight/include        # or your PI install path
ONNX_DIR=/usr/local/onnxruntime        # ONNX Runtime installation
```

---

## Implementation Phases

### Phase 1: PCL Module Scaffold

**Goal**: Empty module that compiles, loads in PixInsight, and shows a basic UI.

#### Step 1.1: Create Directory Structure
```bash
NukeX/
├── src/
│   ├── NukeXModule.h
│   ├── NukeXModule.cpp
│   ├── NukeXProcess.h
│   ├── NukeXProcess.cpp
│   ├── NukeXInstance.h
│   ├── NukeXInstance.cpp
│   ├── NukeXInterface.h
│   ├── NukeXInterface.cpp
│   ├── NukeXParameters.h
│   └── NukeXParameters.cpp
├── engine/
│   └── (empty, populated later)
├── resources/
│   ├── models/
│   └── icons/
├── Makefile
└── NukeX.pidoc
```

#### Step 1.2: Implement Core PCL Files

**Order of implementation:**

1. **NukeXParameters.h/.cpp** - Define all parameters first
   - Start with minimal set: `p_autoSegment`, `p_globalContrast`, `p_saturationBoost`
   - Add more as features are built

2. **NukeXModule.h/.cpp** - Module definition
   - Module metadata (name: "NukeX", version: "1.0.0")
   - Process and interface registration

3. **NukeXProcess.h/.cpp** - Process definition
   - Inherits from `MetaProcess`
   - Creates instances
   - Defines execution context

4. **NukeXInstance.h/.cpp** - Main execution logic
   - `ExecuteOn(View&)` - stub that just copies image
   - Parameter storage
   - Serialization

5. **NukeXInterface.h/.cpp** - User interface
   - Minimal dialog with placeholder controls
   - Real-time preview stub

#### Step 1.3: Create Makefile
```makefile
# Adapt from spec, ensure paths are correct for your system
```

#### Step 1.4: Validation
- [ ] Module compiles without errors
- [ ] Module loads in PixInsight (Process > NukeX appears)
- [ ] Dialog opens with basic controls
- [ ] Preview updates (even if just pass-through)

---

### Phase 2: Stretch Algorithm Library

**Goal**: Implement all 11 stretch algorithms with consistent interface.

#### Step 2.1: Create Algorithm Interface

**File: `engine/IStretchAlgorithm.h`**
```cpp
class IStretchAlgorithm {
public:
    virtual ~IStretchAlgorithm() = default;
    virtual double Apply(double value) const = 0;
    virtual String Name() const = 0;
    virtual void SetParameters(const PropertyArray& params) = 0;
    virtual PropertyArray GetParameters() const = 0;
    virtual void AutoConfigure(const RegionStatistics& stats) = 0;
};
```

#### Step 2.2: Implement Algorithms (Priority Order)

Start with simpler algorithms, build complexity:

| Priority | Algorithm | Complexity | Notes |
|----------|-----------|------------|-------|
| 1 | MTFStretch | Simple | Classic midtones transfer |
| 2 | HistogramStretch | Simple | Shadows/mids/highlights clipping |
| 3 | ArcSinhStretch | Medium | asinh(x * softness) / asinh(softness) |
| 4 | LogStretch | Medium | log(1 + scale*x) / log(1 + scale) |
| 5 | GHStretch | Complex | Full GHS implementation |
| 6 | LumptonStretch | Medium | SDSS HDR style |
| 7 | OTSStretch | Medium | Optimal transfer |
| 8 | RNCStretch | Medium | Color preservation focus |
| 9 | SASStretch | Complex | Noise-aware, iterative |
| 10 | PhotometricStretch | Medium | Photometric accuracy |
| 11 | VeraluxStretch | Complex | Film response curves |

**Files per algorithm:**
```
engine/algorithms/
├── MTFStretch.h
├── MTFStretch.cpp
├── ArcSinhStretch.h
├── ArcSinhStretch.cpp
... (one pair per algorithm)
```

#### Step 2.3: Create Stretch Library Manager

**File: `engine/StretchLibrary.h/.cpp`**
- Factory pattern for creating algorithms by name/enum
- Registry of all available algorithms
- Parameter presets

#### Step 2.4: Validation
- [ ] Unit tests: known input -> expected output for each algorithm
- [ ] Visual test: apply each to same image, compare results
- [ ] Match PixInsight's built-in equivalents (MTF, HT)

---

### Phase 3: Basic Single-Stretch Integration

**Goal**: Apply a single stretch to an entire image via the UI.

#### Step 3.1: Wire UI to Algorithm Library
- Dropdown to select algorithm
- Sliders for algorithm parameters
- Real-time preview updates

#### Step 3.2: Implement ExecuteOn()
```cpp
void NukeXInstance::ExecuteOn(View& view) {
    // 1. Get selected algorithm from parameters
    // 2. Configure algorithm with current params
    // 3. Apply to every pixel
    // 4. Update view
}
```

#### Step 3.3: Implement Real-Time Preview
- Use PixInsight's preview system
- Debounce parameter changes
- Show processing time

#### Step 3.4: Validation
- [ ] Each algorithm produces visible stretch
- [ ] Preview matches execute result
- [ ] Parameters persist when dialog reopens
- [ ] Can save/load process icon

---

### Phase 4: Region Statistics Engine

**Goal**: Compute per-region statistics to inform algorithm selection.

#### Step 4.1: Implement Statistics Data Structure

**File: `engine/RegionStatistics.h`**
```cpp
struct RegionStatistics {
    double min, max, mean, median, stdDev;
    double mad;  // Median Absolute Deviation
    double peakLocation, peakWidth;
    double dynamicRange;  // log scale
    double clippingPct;   // pixels at 0 or 1
    double snrEstimate;
    Histogram histogram;  // 65536 bins
};
```

#### Step 4.2: Implement Histogram Engine

**File: `engine/HistogramEngine.h/.cpp`**
- Fast histogram computation
- Support for masked regions
- 65536 bins for 16-bit equivalent precision

#### Step 4.3: Implement Region Analyzer

**File: `engine/RegionAnalyzer.h/.cpp`**
- Compute all stats for a given mask
- Thread-safe for parallel computation
- Cache results

#### Step 4.4: Validation
- [ ] Stats match PixInsight's Statistics tool
- [ ] Handles edge cases (empty mask, single-value region)
- [ ] Performance: <100ms for typical image

---

### Phase 5: ONNX Segmentation Integration

**Goal**: Load and run AI model to generate region masks.

#### Step 5.1: ONNX Runtime Wrapper

**File: `engine/ONNXInference.h/.cpp`**
- Initialize ONNX Runtime
- Load model from file
- Run inference on image data
- Handle errors gracefully

#### Step 5.2: Segmentation Engine

**File: `engine/Segmentation.h/.cpp`**
- Preprocess image (resize to model input size)
- Run inference
- Post-process output to masks (9 channels, one per region class)
- Resize masks back to original image size

#### Step 5.3: Mock Model for Development
- Create simple threshold-based "model" for testing
- Separates stars (bright), nebula (mid), background (dark)
- Allows full pipeline testing without real model

#### Step 5.4: Model Selection

**Initial approach:**
1. Use SAM (Segment Anything) with automatic prompting
2. Fine-tune for astro data later
3. Fallback to simpler U-Net if SAM too slow

**File: `engine/SegmentationModel.h`** - Abstract base class

#### Step 5.5: Validation
- [ ] ONNX Runtime initializes without error
- [ ] Mock model produces reasonable masks
- [ ] Real model loads (when available)
- [ ] Masks visualize correctly in UI

---

### Phase 6: Stretch Selector (Rules Engine)

**Goal**: Automatically choose best algorithm for each region.

#### Step 6.1: Implement Selection Rules

**File: `engine/SelectionRules.h/.cpp`**

```cpp
// Region Class -> Recommended Algorithm + Parameters
static const std::map<RegionClass, AlgorithmRecommendation> DefaultRules = {
    {RegionClass::StarCore,     {Algorithm::ArcSinh, {{"softness", 0.3}}}},
    {RegionClass::StarHalo,     {Algorithm::GHS, {{"HP", 0.8}}}},
    {RegionClass::NebulaBright, {Algorithm::GHS, {}}},
    {RegionClass::NebulaFaint,  {Algorithm::Lumpton, {{"Q", 8}}}},
    {RegionClass::DustLane,     {Algorithm::MTF, {}}},
    {RegionClass::GalaxyCore,   {Algorithm::ArcSinh, {{"softness", 0.2}}}},
    {RegionClass::GalaxyHalo,   {Algorithm::Lumpton, {}}},
    {RegionClass::GalaxyArm,    {Algorithm::GHS, {}}},
    {RegionClass::Background,   {Algorithm::MTF, {{"midtones", 0.25}}}}
};
```

#### Step 6.2: Stats-Aware Parameter Tuning
- Use RegionStatistics to adjust algorithm parameters
- High SNR -> more aggressive stretch
- High clipping -> more protection
- Wide dynamic range -> log/arcsinh preference

#### Step 6.3: Implement Stretch Selector

**File: `engine/StretchSelector.h/.cpp`**
- Input: region class + statistics
- Output: configured algorithm instance
- Supports override by user

#### Step 6.4: Validation
- [ ] Different regions get different algorithms
- [ ] Stats influence parameter selection
- [ ] User overrides work

---

### Phase 7: Compositor

**Goal**: Blend all regional stretches into final image.

#### Step 7.1: Implement Blend Engine

**File: `engine/BlendEngine.h/.cpp`**
- For each pixel, compute weighted average of stretched values
- Weights = soft mask values (0-1)
- Handle overlapping regions correctly

```cpp
// Pseudocode
for each pixel (x, y):
    totalWeight = 0
    blendedValue = 0
    for each region r:
        weight = mask[r][x][y]
        stretchedValue = algorithms[r].Apply(originalValue)
        blendedValue += weight * stretchedValue
        totalWeight += weight
    output[x][y] = blendedValue / totalWeight
```

#### Step 7.2: Mask Edge Softening
- Gaussian blur on mask edges
- Configurable blend radius
- Prevent hard transitions

#### Step 7.3: Global Tone Balancing

**File: `engine/ToneMapper.h/.cpp`**
- Optional final curve adjustment
- Histogram equalization pass (subtle)
- Black point / white point normalization

#### Step 7.4: LRGB Processor

**File: `engine/LRGBProcessor.h/.cpp`**
- Separate luminance and chrominance processing
- Apply stretch to L only, preserve color ratios
- Recombine with optional saturation boost

#### Step 7.5: Validation
- [ ] Blending produces smooth transitions
- [ ] No visible seams between regions
- [ ] Colors preserved (not muddy or oversaturated)
- [ ] LRGB mode matches expected behavior

---

### Phase 8: Full UI Implementation

**Goal**: Complete user interface with all controls.

#### Step 8.1: Preview Modes
- Before/After split view
- Region map visualization (color-coded masks)
- Individual region preview
- Stretched result

#### Step 8.2: Global Controls
- Contrast slider
- Saturation slider
- Blend radius slider
- LRGB mode toggle

#### Step 8.3: Per-Region Controls
- Enable/disable each region class
- Algorithm dropdown override
- Strength multiplier
- Local contrast
- Local sharpening

#### Step 8.4: Advanced Panel
- Show/hide detailed statistics per region
- Algorithm-specific parameter access
- Mask export option

#### Step 8.5: Validation
- [ ] All controls responsive
- [ ] Preview updates in real-time (<500ms)
- [ ] Settings persist correctly
- [ ] Keyboard shortcuts work

---

## Testing Strategy

### Unit Tests
- Each stretch algorithm: known input -> expected output
- Statistics computation: compare to known values
- Mask blending: edge cases and normal cases

### Integration Tests
- Full pipeline on sample images
- Compare to manual stretch results
- Performance benchmarks

### Test Images
Create/collect test images:
1. Star field only (test star protection)
2. Bright nebula (M42 type)
3. Faint nebula (IFN type)
4. Galaxy (core + halo)
5. Mixed field (stars + nebula + dust)
6. Linear synthetic gradient (verify math)

---

## Performance Considerations

### Targets
- Segmentation: <5 seconds on 4K image
- Statistics: <1 second per region
- Stretch + blend: <10 seconds total
- Preview update: <500ms

### Optimization Strategies
1. **Downsample for segmentation**: Run model on 512x512, upscale masks
2. **Parallel processing**: OpenMP for pixel loops
3. **Lazy computation**: Only recompute changed regions
4. **GPU acceleration**: ONNX Runtime CUDA provider (optional)
5. **Mask caching**: Store computed masks, reuse if image unchanged

---

## File Implementation Order

Recommended order for Claude Code sessions:

### Session 1: Scaffold
1. `NukeXParameters.h/.cpp`
2. `NukeXModule.h/.cpp`
3. `NukeXProcess.h/.cpp`
4. `NukeXInstance.h/.cpp` (stub)
5. `NukeXInterface.h/.cpp` (minimal)
6. `Makefile`

### Session 2: Core Algorithms
1. `engine/IStretchAlgorithm.h`
2. `engine/algorithms/MTFStretch.h/.cpp`
3. `engine/algorithms/ArcSinhStretch.h/.cpp`
4. `engine/algorithms/HistogramStretch.h/.cpp`
5. `engine/StretchLibrary.h/.cpp`

### Session 3: More Algorithms
1. `engine/algorithms/GHStretch.h/.cpp`
2. `engine/algorithms/LogStretch.h/.cpp`
3. `engine/algorithms/LumptonStretch.h/.cpp`
4. Remaining algorithms

### Session 4: Statistics
1. `engine/RegionStatistics.h`
2. `engine/HistogramEngine.h/.cpp`
3. `engine/RegionAnalyzer.h/.cpp`

### Session 5: ONNX Integration
1. `engine/ONNXInference.h/.cpp`
2. `engine/SegmentationModel.h`
3. `engine/Segmentation.h/.cpp`

### Session 6: Selection & Composition
1. `engine/SelectionRules.h/.cpp`
2. `engine/StretchSelector.h/.cpp`
3. `engine/BlendEngine.h/.cpp`
4. `engine/ToneMapper.h/.cpp`
5. `engine/LRGBProcessor.h/.cpp`
6. `engine/Compositor.h/.cpp`

### Session 7: Full Pipeline
1. Update `NukeXInstance.cpp` with full pipeline
2. Update `NukeXInterface.cpp` with all controls
3. Testing and debugging

### Session 8: Polish
1. Performance optimization
2. Error handling
3. Documentation (`NukeX.pidoc`)
4. Icon design

---

## Claude Code Prompts

Example prompts for each session:

### Session 1
> "Let's create the PCL module scaffold for NukeX. Start with NukeXParameters.h defining minimal parameters, then NukeXModule.h/.cpp for module registration."

### Session 2
> "Now implement the stretch algorithm interface (IStretchAlgorithm.h) and the first three algorithms: MTF, ArcSinh, and Histogram stretch."

### Session 5
> "Implement the ONNX Runtime wrapper class for loading and running the segmentation model. Include proper error handling and a mock model for testing."

---

## Open Decisions

Track decisions made during implementation:

| Decision | Options | Chosen | Rationale |
|----------|---------|--------|-----------|
| Model input size | 256/512/1024 | TBD | Balance speed vs accuracy |
| GPU requirement | Required/Optional | TBD | User hardware varies |
| Mask precision | 8-bit/16-bit/float | TBD | Memory vs quality |
| Threading model | OpenMP/std::thread | TBD | PCL compatibility |

---

## Resources

- **PCL SDK Docs**: https://pixinsight.com/developer/
- **PCL Class Reference**: https://pixinsight.com/developer/pcl/
- **ONNX Runtime C++**: https://onnxruntime.ai/docs/api/c/
- **GHS Math Reference**: https://ghsastro.co.uk/
- **Sample PCL Modules**: Study existing PI modules for patterns

---

*Document Version: 1.0*
*Created: January 2026*
*For use with: Claude Code*
