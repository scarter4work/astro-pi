# NukeX - Intelligent Region-Aware Stretch for PixInsight

> "NukeX - Will blow your socks off!"

## Executive Summary

NukeX is a PixInsight PCL module that revolutionizes image stretching by using AI-driven semantic segmentation to identify distinct regions in astrophotography images and apply optimally-selected stretch algorithms to each region independently. Unlike traditional global stretches (including PixInsight's STF, HistogramTransformation, and even GHS), NukeX understands that star cores, faint nebulosity, dust lanes, and galaxy halos each require different treatment.

The result: One-click stretches that rival hours of manual luminosity masking and careful curve work, without the blown cores and crushed faint detail that plague existing "auto stretch" solutions.

## The Problem

Current stretch tools apply a single mathematical transformation to all pixels:

- **STF/AutoSTF**: Global midtones transfer function - blows out bright cores
- **HistogramTransformation**: Single curve for everything
- **GHS (Generalized Hyperbolic Stretch)**: Sophisticated but still one curve globally
- **Siril's Asinh/GHS**: Same limitation
- **"Nuke Button" approaches**: Optimize for histogram shape, not regional fidelity

Experienced imagers work around this by:
1. Creating luminosity masks (bright, midtone, dark)
2. Creating feature masks (stars, nebula, background)
3. Applying different stretches to each masked region
4. Carefully blending with smooth transitions
5. Iterating for hours

NukeX automates this entire workflow with AI.

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        NukeX Module                              │
├─────────────────────────────────────────────────────────────────┤
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐  │
│  │  Segmentation   │  │ Region Analyzer │  │ Stretch Library │  │
│  │     Engine      │  │                 │  │                 │  │
│  │  (ONNX/SAM)     │  │  Per-mask stats │  │  All algorithms │  │
│  └────────┬────────┘  └────────┬────────┘  └────────┬────────┘  │
│           │                    │                    │           │
│           v                    v                    v           │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │                    Stretch Selector                         ││
│  │         (Rules engine / ML model for algorithm choice)      ││
│  └─────────────────────────────────────────────────────────────┘│
│                              │                                   │
│                              v                                   │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │                      Compositor                              ││
│  │    (Blending, transitions, global optimization, L+RGB)      ││
│  └─────────────────────────────────────────────────────────────┘│
│                              │                                   │
│                              v                                   │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │                   NukeX Interface                            ││
│  │         (PI native UI, real-time preview, controls)         ││
│  └─────────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────┘
```

## Component Specifications

### 1. Segmentation Engine

**Purpose**: Identify semantically distinct regions in the linear (unstretched) image.

**Region Classes**:
| Class ID | Name | Description |
|----------|------|-------------|
| 0 | background | Sky background, should stretch to neutral dark gray |
| 1 | star_core | Bright stellar cores, need aggressive protection |
| 2 | star_halo | Diffraction/seeing halos around stars |
| 3 | nebula_bright | Bright emission/reflection regions (e.g., M42 core) |
| 4 | nebula_faint | Faint outer nebulosity, IFN, needs maximum lift |
| 5 | dust_lane | Dark nebulae, absorption features |
| 6 | galaxy_core | Bright galactic nuclei |
| 7 | galaxy_halo | Faint outer galactic regions |
| 8 | galaxy_arm | Spiral structure, distinct from halo |

**Implementation**:
- Use ONNX Runtime for inference (C++ integration)
- Initial model: Segment Anything (SAM) with astro fine-tuning
- Fallback: U-Net trained specifically on astro data
- Input: Linear FITS/XISF image (32-bit float)
- Output: Multi-channel mask image (one channel per class, float 0-1 for soft edges)

**Files**:
```
engine/
├── Segmentation.h
├── Segmentation.cpp
├── ONNXInference.h      // ONNX Runtime wrapper
├── ONNXInference.cpp
└── SegmentationModel.h  // Abstract base for different models
```

### 2. Region Analyzer

**Purpose**: Compute statistics for each masked region to inform algorithm selection.

**Per-Region Metrics**:
- Histogram (full resolution, 65536 bins for 16-bit equiv)
- Min, Max, Mean, Median, StdDev
- MAD (Median Absolute Deviation) for robust noise estimation
- Peak location and width
- Dynamic range (log scale)
- Clipping percentage (pixels at 0 or 1)
- SNR estimate

**Files**:
```
engine/
├── RegionAnalyzer.h
├── RegionAnalyzer.cpp
├── RegionStatistics.h   // Data structure for stats
└── HistogramEngine.h    // Fast histogram computation
```

### 3. Stretch Library

**Purpose**: Implementation of all supported stretch algorithms with consistent interface.

**Algorithms to Implement**:

| Algorithm | Best For | Key Parameters |
|-----------|----------|----------------|
| **Midtones Transfer Function (MTF)** | General purpose | midtones balance |
| **Histogram Transformation** | Fine control | shadows, midtones, highlights clip points |
| **Generalized Hyperbolic Stretch (GHS)** | Balanced stretch | D (stretch factor), b (symmetry), SP (shadow protection), HP (highlight protection), LP (local intensity) |
| **Arcsinh Stretch** | High dynamic range, cores | softness, black point |
| **Logarithmic Stretch** | Faint detail | scale factor, black point |
| **Lumpton (Lupton)** | SDSS-style HDR | Q (softening), alpha, minimum |
| **RNC (Rnc Color Stretch)** | Color preservation | stretch factor, color boost |
| **Photometric Color Calibration Stretch** | Calibrated data | maintains photometric accuracy |
| **OTS (Optimal Transfer Stretch)** | Automatic | target median, black point |
| **SAS (Statistical Adaptive Stretch)** | Noise-aware | SNR threshold, iterations |
| **Veralux** | Film-like response | exposure, contrast, saturation curves |

**Interface**:
```cpp
class IStretchAlgorithm {
public:
    virtual ~IStretchAlgorithm() = default;
    
    // Apply stretch to pixel value (0-1 linear input, 0-1 stretched output)
    virtual double Apply(double value) const = 0;
    
    // Apply to entire image region with mask
    virtual void ApplyToRegion(Image& img, const Image& mask) const;
    
    // Get algorithm name for UI
    virtual String Name() const = 0;
    
    // Parameter access
    virtual void SetParameters(const PropertyArray& params) = 0;
    virtual PropertyArray GetParameters() const = 0;
    
    // Suggest parameters based on region statistics
    virtual void AutoConfigure(const RegionStatistics& stats) = 0;
};
```

**Files**:
```
engine/
├── StretchLibrary.h
├── IStretchAlgorithm.h
├── algorithms/
│   ├── MTFStretch.h/.cpp
│   ├── HistogramStretch.h/.cpp
│   ├── GHStretch.h/.cpp
│   ├── ArcSinhStretch.h/.cpp
│   ├── LogStretch.h/.cpp
│   ├── LumptonStretch.h/.cpp
│   ├── RNCStretch.h/.cpp
│   ├── PhotometricStretch.h/.cpp
│   ├── OTSStretch.h/.cpp
│   ├── SASStretch.h/.cpp
│   └── VeraluxStretch.h/.cpp
```

### 4. Stretch Selector

**Purpose**: Choose optimal algorithm and parameters for each region based on its class and statistics.

**Selection Logic** (Initial Rules-Based Approach):

```
Region Class          | Primary Algorithm | Fallback      | Key Consideration
----------------------|-------------------|---------------|-------------------
star_core             | ArcSinh           | GHS (HP high) | Prevent blowout
star_halo             | GHS               | MTF           | Smooth falloff
nebula_bright         | GHS               | RNC           | Color preservation
nebula_faint          | Log / Lumpton     | SAS           | Maximum lift, noise aware
dust_lane             | MTF               | GHS           | Maintain darkness, detail
galaxy_core           | ArcSinh           | GHS           | Like star cores
galaxy_halo           | Lumpton           | GHS           | Faint detail
galaxy_arm            | GHS               | RNC           | Structure + color
background            | MTF               | Linear clip   | Neutral, noise floor
```

**Future Enhancement**: Train ML model on user-rated results to learn optimal selection.

**Files**:
```
engine/
├── StretchSelector.h
├── StretchSelector.cpp
├── SelectionRules.h     // Rules engine
└── SelectionModel.h     // Future ML model
```

### 5. Compositor

**Purpose**: Blend all regional stretches into a cohesive final image.

**Responsibilities**:
- Apply each stretch to its masked region
- Handle overlapping masks with weighted blending
- Ensure smooth transitions (no hard edges)
- Global tone balancing pass
- Optional: Separate L and RGB processing with recombination
- Sharpness and local contrast per region (optional)

**Blending Strategy**:
1. For each pixel, compute weighted average of all applicable stretched values
2. Weights = mask values (soft masks give smooth blending)
3. Apply global tone curve for overall balance
4. Optional luminance/chrominance recombination

**Controls**:
- Blend radius (for mask edge softening)
- Global contrast adjustment
- Saturation preservation/boost
- Per-region fine-tuning overrides

**Files**:
```
engine/
├── Compositor.h
├── Compositor.cpp
├── BlendEngine.h
├── ToneMapper.h         // Global tone balancing
└── LRGBProcessor.h      // Luminance/RGB handling
```

### 6. Orchestrator Agent (Future)

**Purpose**: High-level AI that evaluates the composite result and suggests refinements.

**Capabilities**:
- Compare result to "reference" well-stretched images
- Identify problem areas (blown regions, muddy backgrounds, color casts)
- Suggest parameter adjustments
- Iterative refinement loop

**Implementation**: Later phase, possibly using vision-language model via API call or local inference.

## PCL Module Structure

### Standard PCL Files

```
NukeX/
├── NukeXModule.h
├── NukeXModule.cpp
├── NukeXProcess.h
├── NukeXProcess.cpp
├── NukeXInstance.h
├── NukeXInstance.cpp
├── NukeXInterface.h
├── NukeXInterface.cpp
├── NukeXParameters.h
├── NukeXParameters.cpp
├── engine/
│   └── [all engine files as above]
├── resources/
│   ├── models/
│   │   └── segmentation.onnx
│   └── icons/
│       └── NukeX.svg
├── Makefile
└── NukeX.pidoc          // Documentation
```

### NukeXModule

Standard PCL module definition:
- Module metadata (name, version, author)
- Process registration
- Icon registration

### NukeXProcess

The process definition:
- Defines parameters (see below)
- Creates instances
- Defines execution context (global, image window, etc.)

### NukeXInstance

The workhorse:
- Holds all parameter values
- `ExecuteOn(View&)` - main execution
- Preview generation
- Serialization (for process icons, history)

### NukeXInterface

The UI:
- Real-time preview
- Region visualization (show detected regions)
- Algorithm selection overrides
- Per-region parameter tweaking
- Blend controls
- Preview modes (before/after, region map, individual stretches)

### NukeXParameters

All adjustable parameters:

```cpp
// Global
pcl_bool    p_autoSegment;         // Use AI segmentation vs manual masks
pcl_bool    p_autoSelect;          // Auto-select algorithms vs manual
pcl_enum    p_previewMode;         // Before/After/Regions/Individual
pcl_real    p_globalContrast;      // Final contrast adjustment
pcl_real    p_saturationBoost;     // Color saturation
pcl_real    p_blendRadius;         // Mask edge smoothing

// Per-region overrides (array of structs, one per region class)
struct RegionParams {
    pcl_bool  enabled;
    pcl_enum  algorithm;           // Override algorithm selection
    pcl_real  strength;            // Stretch strength multiplier
    pcl_real  contrast;            // Local contrast
    pcl_real  sharpness;           // Local sharpening
    // Algorithm-specific params stored as property array
};
```

## Build System

### Dependencies

- **PixInsight PCL SDK** (latest version)
- **ONNX Runtime** (v1.16+, C++ API)
- **OpenCV** (optional, for image preprocessing if needed)

### Makefile Template

```makefile
# NukeX Makefile for Linux

PCL_DIR = /path/to/PixInsight/include
ONNX_DIR = /path/to/onnxruntime

CXX = g++
CXXFLAGS = -std=c++17 -O3 -fPIC -I$(PCL_DIR) -I$(ONNX_DIR)/include
LDFLAGS = -shared -L$(ONNX_DIR)/lib -lonnxruntime

SOURCES = $(wildcard *.cpp) $(wildcard engine/*.cpp) $(wildcard engine/algorithms/*.cpp)
OBJECTS = $(SOURCES:.cpp=.o)
TARGET = NukeX-pxm.so

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

install: $(TARGET)
	cp $(TARGET) ~/.PixInsight/library/
```

## Development Phases

### Phase 1: Module Scaffold (Week 1)
- [ ] Create all PCL boilerplate files
- [ ] Empty but compiling module
- [ ] Loads in PixInsight
- [ ] Basic UI shell with placeholder controls

### Phase 2: Stretch Library (Week 2-3)
- [ ] Implement IStretchAlgorithm interface
- [ ] Implement all 11 stretch algorithms
- [ ] Unit tests for each algorithm
- [ ] Manual testing with known inputs

### Phase 3: Basic Integration (Week 3-4)
- [ ] Apply single stretch to whole image (baseline functionality)
- [ ] UI controls for algorithm selection and parameters
- [ ] Real-time preview working
- [ ] Matches existing PI tools for validation

### Phase 4: ONNX Integration (Week 4-5)
- [ ] ONNX Runtime wrapper class
- [ ] Load and run pre-trained SAM model
- [ ] Generate region masks from inference output
- [ ] Visualize masks in UI

### Phase 5: Region Analysis (Week 5-6)
- [ ] Implement histogram and statistics computation
- [ ] Per-region stats display in UI
- [ ] Stats-based auto-configuration for algorithms

### Phase 6: Full Pipeline (Week 6-8)
- [ ] Stretch selector with rules engine
- [ ] Compositor with blending
- [ ] Full end-to-end execution
- [ ] Preview modes (regions, individual stretches)

### Phase 7: Training Pipeline (Parallel Track)
- [ ] AstroBin IOTD scraper
- [ ] NASA/ESA image downloader
- [ ] Annotation tool for region labeling
- [ ] Fine-tune segmentation model on astro data
- [ ] Export to ONNX

### Phase 8: Polish (Week 8-10)
- [ ] Performance optimization
- [ ] Documentation (pidoc)
- [ ] Error handling and edge cases
- [ ] Beta testing with real users
- [ ] Icon and branding

## Data Pipeline for ML Training

### Training Data Sources

1. **AstroBin IOTD** (Image of the Day)
   - API: `https://www.astrobin.com/api/v1/imageoftheday/`
   - Requires API key
   - Well-curated, high-quality images
   - Some users provide linear FITS

2. **NASA/ESA Archives**
   - Hubble Legacy Archive: https://hla.stsci.edu/
   - ESA Science Archive: https://www.cosmos.esa.int/
   - Raw and processed data available
   - Open source licensing

3. **User Contributions**
   - Collect linear + stretched pairs from beta users
   - Build community annotation effort

### Annotation Strategy

1. Start with SAM zero-shot on astro images
2. Manually correct obvious errors (50-100 images)
3. Train U-Net on corrected masks
4. Iterate with active learning

### Model Architecture

**Segmentation Model** (U-Net based):
- Input: 512x512 grayscale or RGB (resized, normalized)
- Encoder: ResNet-34 or EfficientNet backbone
- Decoder: Standard U-Net upsampling
- Output: 9-channel softmax (one per region class)
- Export: ONNX format for C++ inference

## UI Mockup

```
┌─────────────────────────────────────────────────────────────────────────┐
│ NukeX                                                          [_][□][X]│
├─────────────────────────────────────────────────────────────────────────┤
│ ┌─────────────────────────────────────────┐ ┌─────────────────────────┐ │
│ │                                         │ │ Preview Mode            │ │
│ │                                         │ │ ○ Before/After          │ │
│ │          [Real-Time Preview]            │ │ ○ Region Map            │ │
│ │                                         │ │ ○ Individual Regions    │ │
│ │                                         │ ├─────────────────────────┤ │
│ │                                         │ │ Global Controls         │ │
│ │                                         │ │ Contrast    [----●----] │ │
│ │                                         │ │ Saturation  [----●----] │ │
│ │                                         │ │ Blend Radius[----●----] │ │
│ └─────────────────────────────────────────┘ ├─────────────────────────┤ │
│                                             │ Regions                 │ │
│ ┌─────────────────────────────────────────┐ │ ☑ Stars      [ArcSinh▾] │ │
│ │ [▼ Region: nebula_faint              ]  │ │ ☑ Nebula Br  [GHS    ▾] │ │
│ ├─────────────────────────────────────────┤ │ ☑ Nebula Fnt [Lumpton▾] │ │
│ │ Algorithm: [Lumpton Stretch         ▾]  │ │ ☑ Dust       [MTF    ▾] │ │
│ │ Strength:  [--------●--]                │ │ ☑ Background [MTF    ▾] │ │
│ │ Q (soft):  [----●------]                │ │ ☑ Galaxy     [GHS    ▾] │ │
│ │ Contrast:  [------●----]                │ ├─────────────────────────┤ │
│ │ Sharpness: [--●--------]                │ │      [  Execute  ]      │ │
│ └─────────────────────────────────────────┘ │      [  Preview  ]      │ │
│                                             │      [   Reset   ]      │ │
└─────────────────────────────────────────────┴─────────────────────────┘
```

## Success Metrics

1. **Quality**: Side-by-side with manual stretch by experienced imagers - NukeX should be comparable or better in 80%+ of cases
2. **Speed**: Full stretch in <30 seconds on typical hardware
3. **Usability**: One-click default mode produces good results; power users can tweak
4. **Adoption**: Positive reception in PixInsight forums, Cloudy Nights, etc.

## Open Questions

1. **Model size vs quality tradeoff**: Smaller model = faster inference but worse segmentation. Target inference time?
2. **GPU acceleration**: ONNX Runtime supports CUDA. Require GPU or CPU-only fallback?
3. **Mask resolution**: Full image resolution masks or downsample for speed?
4. **Iteration**: Should NukeX support "apply and refine" workflow, or one-shot?
5. **Integration**: Should it chain with other tools (e.g., output masks for use in other processes)?

## References

- PixInsight PCL Documentation: https://pixinsight.com/developer/
- ONNX Runtime C++ API: https://onnxruntime.ai/docs/api/c/
- Segment Anything (SAM): https://segment-anything.com/
- GHS Documentation: https://ghsastro.co.uk/
- AstroBin API: https://www.astrobin.com/api/

---

*Document Version: 1.0*
*Created: January 2026*
*Author: Scott + Claude collaboration*
*Project Codename: NukeX*
*Tagline: "Will blow your socks off!"*
