# BayesianAstro

Distribution-aware image stacking for PixInsight with per-pixel confidence scoring.

## Overview

BayesianAstro takes a fundamentally different approach to image stacking:

- **Traditional stacking**: Mean/median/sigma-clip reduces N frames to 1 value, discarding statistical information
- **BayesianAstro**: Preserves full per-pixel distribution, classifies behavior, computes confidence scores, makes intelligent fusion decisions

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    React UI (Qt WebView)                     │
│         File selection, parameters, progress display         │
└─────────────────────────┬───────────────────────────────────┘
                          │ QWebChannel
┌─────────────────────────▼───────────────────────────────────┐
│              C++ PixInsight Module (PCL)                     │
│    Process registration, image I/O, Julia embedding          │
└─────────────────────────┬───────────────────────────────────┘
                          │ Julia C API
┌─────────────────────────▼───────────────────────────────────┐
│                    Julia Core                                │
│    Welford's algorithm, classification, fusion, CUDA.jl     │
└─────────────────────────────────────────────────────────────┘
```

## Directory Structure

```
BayesianAstro/
├── julia/              # Julia core library
│   ├── src/
│   │   ├── BayesianAstro.jl
│   │   ├── statistics/     # Welford, classification, confidence
│   │   ├── fusion/         # MLE, confidence-weighted, lucky, multi-scale
│   │   ├── gpu/            # CUDA.jl kernels
│   │   └── ...
│   └── test/
├── cpp/                # PixInsight C++ module
│   ├── CMakeLists.txt
│   ├── include/
│   └── src/
└── ui/                 # React frontend
    ├── package.json
    └── src/
```

## Key Features

### Statistical Engine
- **Welford's Algorithm**: Single-pass, numerically stable computation of mean, variance, skewness, kurtosis
- **Distribution Classification**: Gaussian, Poisson, Bimodal, Skewed (cosmic rays), Uniform (saturated)
- **Confidence Scoring**: 0-1 score based on sample count, variance, distribution type, outlier indicators

### Fusion Strategies
- **MLE**: Maximum Likelihood Estimation (mean for Gaussian)
- **Confidence-Weighted**: Weight by inverse variance and confidence
- **Lucky Imaging**: Per-pixel selection from best frame
- **Multi-Scale**: Different strategies at different spatial frequencies

### GPU Acceleration
- CUDA.jl support for parallel processing
- Target hardware: NVIDIA RTX 5070 Ti (Blackwell, 16GB VRAM)
- Streams frames from disk - handles stacks larger than RAM

## Building

### Prerequisites
- PixInsight SDK (requires certified developer access)
- Julia 1.10+
- Qt6 with WebEngine
- CMake 3.16+
- CUDA Toolkit (optional, for GPU acceleration)

### Build Steps

```bash
# Julia dependencies
cd julia
julia --project=. -e 'using Pkg; Pkg.instantiate()'

# React UI
cd ../ui
npm install
npm run build

# C++ module
cd ../cpp
mkdir build && cd build
cmake .. -DPIXINSIGHT_SDK=/path/to/sdk -DJULIA_DIR=/path/to/julia
make
```

## Status

**In Development** - Awaiting PixInsight certified developer access.

- [x] Julia core implementation
- [x] C++ module skeleton
- [x] React UI structure
- [x] Julia-C++ bindings
- [ ] PCL integration testing
- [ ] GPU kernel optimization
- [ ] PixInsight certification

## License

MIT License
