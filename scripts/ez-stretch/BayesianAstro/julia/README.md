# BayesianAstro

A distribution-aware astrophotography stacking pipeline that preserves statistical information across frames for intelligent fusion decisions.

## Overview

Traditional stacking algorithms (mean, median, sigma-clipped) reduce each pixel across N frames to a single value, losing valuable statistical information. BayesianAstro takes a different approach:

- **Preserves the full distribution** for each pixel across all frames
- **Classifies pixel behavior** (Gaussian, Poisson, bimodal, skewed, etc.)
- **Computes confidence metrics** based on distribution properties
- **Makes intelligent fusion decisions** based on statistical characteristics

## Features

- **Welford's Algorithm**: Numerically stable single-pass computation of mean, variance, skewness, and kurtosis
- **Distribution Classification**: Automatic detection of Gaussian, Poisson, bimodal, and artifact-affected pixels
- **Confidence Scoring**: Per-pixel confidence values indicating reliability of fused results
- **Multiple Fusion Strategies**: MLE, confidence-weighted, lucky imaging, multi-scale
- **GPU Acceleration**: CUDA.jl support for parallel processing (RTX 5070 Ti target)
- **FITS I/O**: Native support for astronomical image formats

## Installation

```julia
using Pkg
Pkg.develop(path="/path/to/BayesianAstro")
```

### Dependencies

The package will automatically install:
- FITSIO.jl - FITS file handling
- CUDA.jl - GPU acceleration (optional)
- Distributions.jl - Statistical distributions
- StatsBase.jl - Statistical functions
- StaticArrays.jl - Performance optimizations
- Optim.jl - Optimization algorithms
- Images.jl - Image processing utilities

## Quick Start

```julia
using BayesianAstro

# Load frames from a directory
stack = load_frame_sequence(find_fits_files("/path/to/frames"))

# Configure processing
config = ProcessingConfig(
    fusion_strategy = CONFIDENCE_WEIGHTED,
    outlier_sigma = 3.0f0,
    use_gpu = true
)

# Process and get results
fused_image, confidence_map = process_stack(stack, config)

# Save outputs
save_fits("output_fused.fits", fused_image)
save_fits("output_confidence.fits", confidence_map)
```

## Architecture

```
BayesianAstro/
├── src/
│   ├── BayesianAstro.jl      # Main module
│   ├── types.jl               # Core data structures
│   ├── io/
│   │   └── FitsIO.jl          # FITS file operations
│   ├── statistics/
│   │   ├── Welford.jl         # Running statistics
│   │   ├── Classification.jl  # Distribution classification
│   │   └── Confidence.jl      # Confidence scoring
│   ├── fusion/
│   │   └── Strategies.jl      # Fusion algorithms
│   ├── gpu/
│   │   └── Kernels.jl         # CUDA implementations
│   ├── pipeline/
│   │   └── Pipeline.jl        # High-level orchestration
│   └── visualization/
│       └── ConfidenceMaps.jl  # Debugging/analysis
└── test/
    └── runtests.jl
```

## Core Types

### PixelDistribution
Accumulated statistics for a single pixel:
- Count, mean, variance (via Welford's)
- Skewness, kurtosis (for distribution classification)
- Min/max values

### PixelResult
Fusion output:
- Fused value
- Confidence (0.0 - 1.0)
- Variance estimate
- Distribution classification

### DistributionType
Classification categories:
- `GAUSSIAN` - Normal signal + noise
- `POISSON` - Low-signal photon counting
- `BIMODAL` - Star + background, artifacts
- `SKEWED_RIGHT` - Cosmic ray / hot pixel
- `SKEWED_LEFT` - Dark artifact
- `UNIFORM` - Saturated / dead pixel
- `UNKNOWN` - Insufficient data

## Fusion Strategies

1. **MLE**: Maximum likelihood estimation (mean for Gaussian)
2. **CONFIDENCE_WEIGHTED**: Weight by inverse variance and confidence
3. **LUCKY**: Per-pixel selection from best frame
4. **MULTISCALE**: Different strategies at different spatial frequencies

## GPU Acceleration

When CUDA.jl is available and a compatible GPU is detected:
- Statistics accumulation runs in parallel across all pixels
- Distribution classification and fusion are parallelized
- Memory streaming enables processing of large stacks

Target hardware: NVIDIA RTX 5070 Ti (Blackwell, 16GB VRAM)

## Development

```julia
# Run tests
using Pkg
Pkg.test("BayesianAstro")
```

## Roadmap

- [x] Core data structures
- [x] Welford's algorithm implementation
- [x] Distribution classification
- [x] Confidence scoring
- [x] Basic fusion strategies
- [x] FITS I/O
- [ ] GPU kernel optimization
- [ ] Multi-scale fusion
- [ ] DSL for pipeline definition
- [ ] PixInsight integration

## License

MIT

## References

- Welford, B. P. (1962). "Note on a method for calculating corrected sums of squares and products"
- Lucky Imaging techniques
- Bayesian image reconstruction methods
