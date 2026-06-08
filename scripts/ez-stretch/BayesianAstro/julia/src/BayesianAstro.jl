"""
    BayesianAstro

A distribution-aware astrophotography stacking pipeline that preserves statistical
information across frames for intelligent fusion decisions.

## Key Features
- Per-pixel statistical distribution tracking via Welford's algorithm
- Confidence metrics derived from distribution properties  
- Multiple fusion strategies (MLE, confidence-weighted, lucky imaging, multi-scale)
- GPU acceleration via CUDA.jl

## Architecture
- `IO`: FITS file reading/writing
- `Statistics`: Distribution accumulation and classification
- `Fusion`: Pixel fusion strategies
- `GPU`: CUDA kernel implementations
- `Pipeline`: High-level processing orchestration
- `Visualization`: Debugging and confidence map generation
"""
module BayesianAstro

using FITSIO
using Distributions
using StatsBase
using StaticArrays
using Optim
using Images

# Conditional CUDA loading
const CUDA_AVAILABLE = Ref(false)
function __init__()
    try
        @eval using CUDA
        if CUDA.functional()
            CUDA_AVAILABLE[] = true
            @info "CUDA available: GPU acceleration enabled"
        else
            @warn "CUDA loaded but not functional: falling back to CPU"
        end
    catch e
        @warn "CUDA not available: falling back to CPU" exception=e
    end
end

# Core types
include("types.jl")

# Submodules - order matters for dependencies
include("io/FitsIO.jl")
include("statistics/Welford.jl")
include("statistics/Classification.jl")
include("statistics/Confidence.jl")
include("fusion/Strategies.jl")

# GPU module must come before Pipeline (Pipeline uses Kernels)
include("gpu/Kernels.jl")

# High-level modules that depend on others
include("pipeline/Pipeline.jl")
include("visualization/ConfidenceMaps.jl")

# Re-export submodule functions
using .FitsIO: load_fits, save_fits, load_frame_sequence, find_fits_files, parse_fits_date
using .Welford: accumulate!, finalize_statistics, reset!, variance, stddev, skewness, kurtosis, merge
using .Classification: classify_distribution, is_artifact_candidate, is_reliable
using .Confidence: compute_confidence, compute_pixel_result, confidence_weight
using .Strategies: fuse_mle, fuse_confidence_weighted, fuse_lucky, fuse_multiscale, select_fusion_strategy
using .Pipeline: process_stack, process_directory
using .ConfidenceMaps: generate_confidence_map, generate_classification_map, apply_confidence_colormap
using .Kernels: is_gpu_available, create_gpu_context, destroy_gpu_context, GPUContext, cpu_accumulate!, cpu_finalize!, cpu_stretch!

# Public API - Types
export PixelDistribution, PixelResult, DistributionType, FrameMetadata, ProcessingConfig
export ImageStack, FusionStrategy

# Distribution type enum values
export GAUSSIAN, POISSON, BIMODAL, SKEWED_RIGHT, SKEWED_LEFT, UNIFORM, UNKNOWN

# Fusion strategy enum values
export MLE, CONFIDENCE_WEIGHTED, LUCKY, MULTISCALE

# I/O functions
export load_fits, save_fits, load_frame_sequence, find_fits_files, parse_fits_date

# Statistics functions
export accumulate!, finalize_statistics, reset!, variance, stddev, skewness, kurtosis

# Classification functions
export classify_distribution, is_artifact_candidate, is_reliable

# Confidence functions
export compute_confidence, compute_pixel_result, confidence_weight

# Fusion functions
export fuse_mle, fuse_confidence_weighted, fuse_lucky, fuse_multiscale, select_fusion_strategy

# Pipeline functions
export process_stack, process_directory

# Visualization functions
export generate_confidence_map, generate_classification_map, apply_confidence_colormap

# GPU functions
export is_gpu_available, create_gpu_context, destroy_gpu_context, GPUContext
export cpu_accumulate!, cpu_finalize!, cpu_stretch!

end # module
