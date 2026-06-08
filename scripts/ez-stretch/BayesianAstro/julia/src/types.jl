"""
Core type definitions for BayesianAstro pipeline.
"""

"""
Classification of per-pixel statistical behavior across frames.
"""
@enum DistributionType begin
    GAUSSIAN = 1          # Normal signal + noise
    POISSON = 2           # Low-signal photon counting regime
    BIMODAL = 3           # Star + background, or artifact
    SKEWED_RIGHT = 4      # Potential cosmic ray / hot pixel
    SKEWED_LEFT = 5       # Potential dark artifact
    UNIFORM = 6           # Saturated or dead pixel
    UNKNOWN = 7           # Insufficient data to classify
end

"""
    PixelDistribution

Accumulated statistics for a single pixel across all frames.
Uses Welford's online algorithm for numerically stable single-pass computation.

# Fields
- `n::UInt16`: Frame count
- `mean::Float32`: Running mean
- `m2::Float32`: Sum of squared deviations (for variance)
- `m3::Float32`: Third central moment (for skewness)
- `m4::Float32`: Fourth central moment (for kurtosis)
- `min::Float32`: Minimum observed value
- `max::Float32`: Maximum observed value
"""
mutable struct PixelDistribution
    n::UInt16
    mean::Float32
    m2::Float32      # For variance: sum((x - mean)^2)
    m3::Float32      # For skewness
    m4::Float32      # For kurtosis
    min::Float32
    max::Float32
    
    function PixelDistribution()
        new(0, 0.0f0, 0.0f0, 0.0f0, 0.0f0, Inf32, -Inf32)
    end
end

"""
    PixelResult

Output structure after fusion, containing the fused value and metadata.

# Fields
- `value::Float32`: Final fused pixel value
- `confidence::Float32`: Confidence score (0.0 - 1.0)
- `variance::Float32`: Estimated variance
- `distribution_type::DistributionType`: Classification of pixel behavior
"""
struct PixelResult
    value::Float32
    confidence::Float32
    variance::Float32
    distribution_type::DistributionType
end

"""
    FrameMetadata

Per-frame quality metrics, computed during registration or pre-analysis.

# Fields
- `filename::String`: Source filename
- `fwhm::Float32`: Seeing estimate (FWHM of stars)
- `background::Float32`: Sky background level
- `noise::Float32`: Estimated read + sky noise
- `weight::Float32`: Quality weight (computed from other metrics)
- `timestamp::Float64`: Unix timestamp for temporal analysis
"""
struct FrameMetadata
    filename::String
    fwhm::Float32
    background::Float32
    noise::Float32
    weight::Float32
    timestamp::Float64
    
    function FrameMetadata(filename::String; 
                           fwhm=0.0f0, background=0.0f0, 
                           noise=0.0f0, weight=1.0f0, timestamp=0.0)
        new(filename, fwhm, background, noise, weight, timestamp)
    end
end

"""
    FusionStrategy

Available pixel fusion strategies.
"""
@enum FusionStrategy begin
    MLE = 1                  # Maximum likelihood estimation
    CONFIDENCE_WEIGHTED = 2  # Weight by inverse variance and quality
    LUCKY = 3                # Per-pixel lucky imaging (best frame)
    MULTISCALE = 4           # Different strategies at different scales
end

"""
    ProcessingConfig

Pipeline configuration options.

# Fields
- `fusion_strategy::FusionStrategy`: Which fusion algorithm to use
- `confidence_threshold::Float32`: Minimum confidence to include pixel
- `outlier_sigma::Float32`: Sigma threshold for outlier rejection
- `tile_size::Tuple{Int,Int}`: Tile dimensions for GPU memory management
- `use_gpu::Bool`: Whether to attempt GPU acceleration
"""
struct ProcessingConfig
    fusion_strategy::FusionStrategy
    confidence_threshold::Float32
    outlier_sigma::Float32
    tile_size::Tuple{Int,Int}
    use_gpu::Bool
    
    function ProcessingConfig(;
        fusion_strategy::FusionStrategy = CONFIDENCE_WEIGHTED,
        confidence_threshold::Float32 = 0.1f0,
        outlier_sigma::Float32 = 3.0f0,
        tile_size::Tuple{Int,Int} = (1024, 1024),
        use_gpu::Bool = true
    )
        new(fusion_strategy, confidence_threshold, outlier_sigma, tile_size, use_gpu)
    end
end

"""
    ImageStack

Container for a sequence of frames with associated metadata.
"""
struct ImageStack{T<:AbstractFloat}
    frames::Vector{Matrix{T}}
    metadata::Vector{FrameMetadata}
    width::Int
    height::Int
    channels::Int
    
    function ImageStack(frames::Vector{Matrix{T}}, metadata::Vector{FrameMetadata}) where T
        @assert length(frames) == length(metadata) "Frame count must match metadata count"
        @assert length(frames) > 0 "Must have at least one frame"
        
        height, width = size(frames[1])
        new{T}(frames, metadata, width, height, 1)
    end
end

Base.length(stack::ImageStack) = length(stack.frames)
Base.size(stack::ImageStack) = (stack.height, stack.width, length(stack.frames))
