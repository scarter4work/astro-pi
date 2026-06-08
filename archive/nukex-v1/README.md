# NukeX - Intelligent Region-Aware Stretch for PixInsight

**"Will blow your socks off!"**

NukeX is an advanced image stretching module for PixInsight that uses AI-driven semantic segmentation to identify distinct regions in astrophotography images and apply optimally-selected stretch algorithms to each region independently.

## Features

- **AI-Powered Segmentation**: Automatically identifies 9 distinct region types:
  - Star Cores & Halos
  - Bright & Faint Nebulae
  - Dust Lanes
  - Galaxy Cores, Halos & Arms
  - Sky Background

- **11 Stretch Algorithms**: Each optimized for different content types:
  - MTF (Midtones Transfer Function)
  - Histogram Transformation
  - GHS (Generalized Hyperbolic Stretch)
  - ArcSinh (HDR compression)
  - Logarithmic
  - Lumpton (SDSS-style HDR)
  - RNC (Roger Clark's Color Stretch)
  - Photometric
  - OTS (Optimal Transfer Stretch)
  - SAS (Statistical Adaptive Stretch)
  - Veralux

- **Intelligent Algorithm Selection**: Rule-based selection with adaptive parameter tuning based on region statistics (SNR, dynamic range, median brightness)

- **Seamless Blending**: Soft mask blending with Gaussian feathering for natural transitions between regions

- **LRGB Mode**: Process luminance separately while preserving color information

- **Tone Mapping**: Film-style curves (Reinhard, Filmic, ACES) with local contrast enhancement

- **Real-Time Preview**: Interactive preview with multiple visualization modes

## Requirements

### Build Requirements
- PixInsight Core 1.8.9 or later
- PixInsight Class Library (PCL)
- C++17 compatible compiler
- CMake 3.15+

### Optional Dependencies
- ONNX Runtime 1.14+ (for neural network segmentation)
  - Without ONNX Runtime, NukeX uses threshold-based mock segmentation

## Building

### Linux/macOS

```bash
# Set PixInsight installation path
export PCLDIR=/path/to/pixinsight

# Create build directory
mkdir build && cd build

# Configure
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build
make -j$(nproc)

# Install module
cp NukeX.so $PCLDIR/lib/
```

### Windows (Visual Studio)

```cmd
:: Set PixInsight installation path
set PCLDIR=C:\Program Files\PixInsight

:: Configure
cmake -G "Visual Studio 17 2022" -A x64 ..

:: Build
cmake --build . --config Release

:: Install module
copy Release\NukeX.dll "%PCLDIR%\bin"
```

### With ONNX Runtime

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DONNXRUNTIME_DIR=/path/to/onnxruntime \
         -DNUKEX_USE_ONNX=ON
```

## Usage

1. Open NukeX from **Process > IntensityTransformations > NukeX**

2. Configure processing options:
   - **Algorithm**: Select stretch algorithm or use "Auto" for AI selection
   - **Auto Segmentation**: Enable/disable region detection
   - **LRGB Mode**: Process luminance only (preserves color)

3. Adjust stretch parameters:
   - **Stretch Strength**: Overall intensity multiplier
   - **Black Point**: Clip dark pixels
   - **Blend Radius**: Transition smoothness between regions

4. Enable/disable individual region types as needed

5. Click the **Real-Time Preview** button to see results interactively

6. Apply to the target image

## Architecture

```
src/
├── NukeXModule.cpp          # Module entry point
├── NukeXProcess.cpp         # Process definition
├── NukeXInstance.cpp        # Process instance (execution)
├── NukeXInterface.cpp       # User interface
├── NukeXParameters.cpp      # Process parameters
└── engine/
    ├── StretchAlgorithms.cpp   # Core stretch algorithms
    ├── StretchLibrary.cpp      # Algorithm factory
    ├── RegionStatistics.cpp    # Statistical analysis
    ├── HistogramEngine.cpp     # Histogram computation
    ├── RegionAnalyzer.cpp      # Region classification
    ├── ONNXInference.cpp       # ONNX Runtime wrapper
    ├── SegmentationModel.cpp   # Segmentation models
    ├── Segmentation.cpp        # Segmentation engine
    ├── SelectionRules.cpp      # Algorithm selection rules
    ├── StretchSelector.cpp     # Selection interface
    ├── BlendEngine.cpp         # Region blending
    ├── ToneMapper.cpp          # Tone mapping
    ├── LRGBProcessor.cpp       # LRGB processing
    ├── Compositor.cpp          # Full pipeline
    └── Constants.h             # Shared constants
```

## Algorithm Selection Logic

NukeX uses a priority-based rule engine to select the optimal algorithm for each region:

| Region | Typical Algorithm | Rationale |
|--------|------------------|-----------|
| Star Cores | ArcSinh | HDR compression prevents bloating |
| Star Halos | GHS | Gentle stretch preserves structure |
| Bright Nebula | GHS/MTF | Balanced contrast enhancement |
| Faint Nebula | Veralux/SAS | Maximum faint detail extraction |
| Dust Lanes | Log | Shadow detail without brightening |
| Galaxy Cores | ArcSinh | HDR protection for bright nuclei |
| Galaxy Halos | GHS | Faint structure preservation |
| Galaxy Arms | MTF | Natural contrast for spiral structure |
| Background | MTF (light) | Minimal stretch, noise control |

## Performance Notes

- **Segmentation**: ~200-500ms for 4K images (mock), 1-3s with ONNX model
- **Blending**: Multi-threaded processing scales with CPU cores
- **Preview**: Uses downsampled images (512px max) for responsiveness
- **Memory**: Peak usage ~4x image size during processing

## Troubleshooting

### "Segmentation failed, using mock"
ONNX Runtime is not available or the model file is missing. The module will use threshold-based segmentation instead.

### Slow preview updates
Disable "Auto Segmentation" for faster previews when adjusting parameters.

### Visible region boundaries
Increase the "Blend Radius" parameter for smoother transitions.

## License

MIT License - Copyright (c) 2026 Scott Carter

## Acknowledgments

- PixInsight development team for the PCL framework
- The astrophotography community for algorithm research and feedback
